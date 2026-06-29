#!/usr/bin/env python3
"""
Conservative Polymarket market-making gateway (deposit-wallet / POLY_1271).

Cash-only, post-only, BUY-on-both-sides liquidity:
  for each market we rest  BUY YES @ (mid_yes - half_spread)  and
                            BUY NO  @ (mid_no  - half_spread).
Both are BUYs funded by pUSD — no token inventory needed. A bid on NO at price p
is economically an ask on YES at (1-p), so the pair quotes both sides and earns
liquidity rewards, while post_only guarantees we never cross (never pay taker).

Safety rails: hard caps (size, per-order + total notional, open-order count),
cancel-all on start, cancel-all on exit / SIGINT / SIGTERM, and a dead-man's
switch that cancels everything if market data goes stale.

Signs with sigType 3 via py-clob-client-v2 (funder = deposit wallet). The L2 API
key is the EOA-bound one in /home/ubuntu/.pm_creds.env. Key/creds never printed.

Usage (DRY-RUN by default — builds quotes, posts NOTHING):
    PYTHONPATH=./.pmlibs python3 tools/mm_gateway.py --contracts 0 --size 5 --duration 60
Add --live to actually place orders. Add --duration N to auto-stop+flatten after N s.
"""
import argparse, atexit, json, os, signal, sys, time, urllib.request
from dataclasses import dataclass, field

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".pmlibs"))
from py_clob_client_v2 import (  # noqa: E402
    ClobClient, ApiCreds, OrderArgsV2, OrderType, PartialCreateOrderOptions,
)

HOST = "https://clob.polymarket.com"
CHAIN = 137
DEPOSIT_WALLET = "0x832317706479bb6762741B9b9ba568bb86fFfFF0"
CREDS_FILE = "/home/ubuntu/.pm_creds.env"
KEY_FILE = "/home/ubuntu/.pm_signer_key"
STATUS_FILE = os.path.join(os.path.dirname(__file__), "..", "mm_status.json")


def read_env(path):
    out = {}
    for ln in open(path):
        ln = ln.strip()
        if ln.startswith("export "):
            ln = ln[7:]
        if "=" in ln and not ln.startswith("#"):
            k, v = ln.split("=", 1)
            out[k.strip()] = v.strip().strip('"').strip("'")
    return out


@dataclass
class Caps:
    size: float = 5.0                 # shares per order
    half_spread: float = 0.02         # distance from mid (in price units)
    requote_move: float = 0.01        # re-quote if mid moved by >= this
    max_order_notional: float = 5.0   # $ per single order
    max_total_notional: float = 20.0  # $ across all live orders
    max_open_orders: int = 8
    dms_seconds: float = 20.0         # cancel-all if md stale this long
    interval: float = 5.0             # loop period
    gtd_expiry: float = 120.0         # seconds: orders auto-expire if we go silent


@dataclass
class Market:
    name: str
    yes: str
    no: str


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Gateway:
    def __init__(self, client, markets, caps: Caps, live: bool, skew: bool = True):
        self.c = client
        self.markets = markets
        self.caps = caps
        self.live = live
        self.skew = skew            # don't add to a side we already hold inventory in
        self.last_md_ok = time.time()
        self.last_mid = {}          # token_id -> last mid we quoted around
        self._flattened = False

    def held_tokens(self):
        """Tokens we currently hold inventory in (asset_id -> size), via data-api."""
        if not self.skew:
            return {}
        try:
            url = f"https://data-api.polymarket.com/positions?user={DEPOSIT_WALLET}"
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            data = json.loads(urllib.request.urlopen(req, timeout=8).read())
            return {p.get("asset"): float(p.get("size", 0) or 0)
                    for p in (data or []) if float(p.get("size", 0) or 0) > 0.5}
        except Exception:
            return {}

    # ---- safety ----------------------------------------------------------
    def flatten(self, reason=""):
        if self._flattened:
            return
        try:
            r = self.c.cancel_all()
            log(f"FLATTEN ({reason}): cancel_all -> {r}")
        except Exception as e:
            log(f"FLATTEN ({reason}): cancel_all ERROR {e!r}")
        self._flattened = True

    def open_orders(self):
        try:
            return self.c.get_open_orders() or []
        except Exception as e:
            log(f"get_open_orders ERROR {e!r}")
            return None

    # ---- pricing ---------------------------------------------------------
    @staticmethod
    def round_tick(price, tick):
        steps = round(price / tick)
        p = steps * tick
        return max(tick, min(1 - tick, round(p, 6)))

    def mid(self, token):
        m = self.c.get_midpoint(token)
        return float(m["mid"]) if isinstance(m, dict) else float(m)

    # ---- one quoting cycle ----------------------------------------------
    def cycle(self):
        caps = self.caps
        # 1) dead-man's switch on stale market data
        if time.time() - self.last_md_ok > caps.dms_seconds:
            self.flatten("dead-man's switch: market data stale")
            self._flattened = False  # allow recovery on next good tick
            return

        # 2) snapshot current live orders for caps + reconciliation
        live_orders = self.open_orders()
        if live_orders is None:
            return  # md error already logged; DMS will trip if it persists
        total_notional = sum(float(o["price"]) * float(o["original_size"]) for o in live_orders)

        held = self.held_tokens()      # inventory skew: never add to a side we hold
        desired = []   # (token, side, price, size)
        for mk in self.markets:
            try:
                for token in (mk.yes, mk.no):
                    if held.get(token, 0) > 0.5:
                        log(f"skew: hold {held[token]:.0f} of tok…{token[-6:]}, skip its BUY")
                        continue
                    mid = self.mid(token)
                    tick = float(self.c.get_tick_size(token))
                    bid = self.round_tick(mid - caps.half_spread, tick)
                    if bid < tick:
                        continue
                    desired.append((token, "BUY", bid, caps.size, tick))
                self.last_md_ok = time.time()
            except Exception as e:
                log(f"md error {mk.name}: {e!r}")
                continue

        # 3) decide whether to re-quote: only if mid moved enough or nothing live
        need = not live_orders
        for token, _, price, _, _ in desired:
            prev = self.last_mid.get(token)
            if prev is None or abs(price - prev) >= caps.requote_move:
                need = True
        if not need:
            log(f"hold: {len(live_orders)} live, ${total_notional:.2f} notional")
            return

        # 4) cap checks
        new_notional = sum(p * s for _, _, p, s, _ in desired)
        if any(p * s > caps.max_order_notional for _, _, p, s, _ in desired):
            log("SKIP: an order exceeds max_order_notional"); return
        if new_notional > caps.max_total_notional:
            log(f"SKIP: total notional ${new_notional:.2f} > cap ${caps.max_total_notional}"); return
        if len(desired) > caps.max_open_orders:
            log(f"SKIP: {len(desired)} orders > max_open_orders"); return

        # 5) replace: cancel all, then place the fresh quotes
        log(f"re-quote: cancelling {len(live_orders)} and placing {len(desired)} "
            f"(${new_notional:.2f} notional)")
        if self.live and live_orders:
            try: self.c.cancel_all()
            except Exception as e: log(f"cancel_all ERROR {e!r}")

        for token, side, price, size, tick in desired:
            tag = f"{side} {size}@{price:.2f} tok…{token[-6:]}"
            if not self.live:
                log(f"  DRY  {tag}"); continue
            try:
                neg = self.c.get_neg_risk(token)
                ts = self.c.get_tick_size(token)
                # GTD: the order auto-expires after gtd_expiry seconds. If this
                # process (or the whole box) dies, resting orders die with it
                # within ~gtd_expiry — they can never sit naked overnight again.
                exp = int(time.time() + self.caps.gtd_expiry)
                resp = self.c.create_and_post_order(
                    OrderArgsV2(token_id=token, price=price, size=size, side=side,
                                expiration=exp),
                    options=PartialCreateOrderOptions(tick_size=ts, neg_risk=neg),
                    order_type=OrderType.GTD, post_only=True)
                ok = isinstance(resp, dict) and resp.get("success")
                log(f"  {'OK ' if ok else 'ERR'} {tag} -> {resp}")
            except Exception as e:
                log(f"  ERR {tag} -> {e!r}")
            self.last_mid[token] = price

        self.write_status(live_orders, new_notional)

    def write_status(self, live_orders, notional):
        try:
            json.dump({
                "ts": int(time.time()), "live": self.live,
                "deposit_wallet": DEPOSIT_WALLET,
                "open_orders": len(live_orders), "target_notional": round(notional, 2),
                "markets": [m.name for m in self.markets],
            }, open(STATUS_FILE, "w"))
        except Exception:
            pass


def load_markets(indices):
    cfg = json.load(open(os.path.join(os.path.dirname(__file__), "..", "config.live.json")))
    out = []
    for i in indices:
        c = cfg["contracts"][i]
        out.append(Market(c["name"][:40], c["token_id_yes"], c["token_id_no"]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--contracts", default="0", help="comma indices into config.live.json contracts")
    ap.add_argument("--size", type=float, default=5.0)
    ap.add_argument("--half-spread", type=float, default=0.02)
    ap.add_argument("--max-order-notional", type=float, default=5.0)
    ap.add_argument("--max-total-notional", type=float, default=20.0)
    ap.add_argument("--interval", type=float, default=5.0)
    ap.add_argument("--gtd-expiry", type=float, default=120.0,
                    help="seconds until a resting order auto-expires (dead-process safety)")
    ap.add_argument("--duration", type=float, default=0.0, help="auto stop+flatten after N s (0=forever)")
    ap.add_argument("--live", action="store_true", help="actually place orders (default: dry-run)")
    ap.add_argument("--no-skew", action="store_true", help="disable inventory skew (allow adding to held sides)")
    a = ap.parse_args()

    env = read_env(CREDS_FILE)
    key = open(KEY_FILE).read().strip()
    client = ClobClient(host=HOST, chain_id=CHAIN, key=key,
                        creds=ApiCreds(env["PM_API_KEY"], env["PM_API_SECRET"], env["PM_API_PASSPHRASE"]),
                        signature_type=3, funder=DEPOSIT_WALLET)
    caps = Caps(size=a.size, half_spread=a.half_spread,
                max_order_notional=a.max_order_notional, max_total_notional=a.max_total_notional,
                interval=a.interval, gtd_expiry=a.gtd_expiry)
    markets = load_markets([int(x) for x in a.contracts.split(",")])

    log(f"mode={'LIVE' if a.live else 'DRY-RUN'} | funder={DEPOSIT_WALLET} | "
        f"markets={[m.name for m in markets]} | size={caps.size} hs={caps.half_spread} "
        f"GTD={caps.gtd_expiry:.0f}s | caps: order≤${caps.max_order_notional} total≤${caps.max_total_notional}")

    gw = Gateway(client, markets, caps, a.live, skew=not a.no_skew)
    log(f"inventory skew: {'ON' if gw.skew else 'OFF'}")
    # cancel-all on every exit path
    atexit.register(lambda: gw.flatten("exit") if a.live else None)
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: (gw.flatten("signal"), sys.exit(0)))

    if a.live:
        gw.flatten("startup clear"); gw._flattened = False  # clear orphans, then resume

    start = time.time()
    try:
        while True:
            gw.cycle()
            if a.duration and time.time() - start >= a.duration:
                log(f"duration {a.duration}s reached"); break
            time.sleep(caps.interval)
    finally:
        if a.live:
            gw.flatten("shutdown")
            oo = gw.open_orders()
            log(f"final open orders: {len(oo) if oo is not None else '?'}")


if __name__ == "__main__":
    main()
