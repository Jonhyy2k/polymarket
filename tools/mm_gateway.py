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

Automated fill-handling (--fill-handling, OFF by default): when a fill legs us
(we hold one side, not a hedged set) the bot's default is to COMPLETE THE HEDGE
(keep quoting the other side, hold the leg, rewards cushion the wait). Around that
it runs a safety state machine — SCRATCH (exit flat if the leg moved favourably),
STOP (cut a bleeding leg), DTE-FLATTEN (don't carry an un-hedged leg into
resolution) — flattening with a marketable FAK at the bid. A complete (hedged) set
is risk-free and is never touched. Exits are double-gated: --fill-handling enables
the *logic*, --arm-exits enables actually *placing* the SELLs (else log-only).

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
class FillPolicy:
    """Automated handling of a *legged* fill (we hold one side, not a hedged set).

    Default behaviour when legged = COMPLETE THE HEDGE: keep quoting the OTHER side and
    hold the leg (ongoing LP rewards cushion the wait) — the proven play (it locked the
    rials set +$0.80 risk-free). These thresholds define the SAFETY band around that:
    outside the band we flatten the leg with a marketable FAK at the bid. The middle band
    [fill−stop_loss, fill+scratch_target] is "hold & complete the hedge".

    Off by default; even when enabled, exits are only PLACED with --arm-exits (a second
    gate, because selling inventory is higher-stakes than resting a bid). Tune the numbers
    against logs/fill_telemetry.csv mark-outs once live in a GOOD-exit market.
    """
    enabled: bool = False          # master switch (default OFF → plain skew behaviour)
    scratch_target: float = 0.10   # bid ≥ fill+this → take the favourable exit, go flat
    stop_loss: float = 0.12        # bid ≤ fill−this → cut the loss, go flat
    dte_flatten_days: int = 1      # DTE ≤ this → flatten any un-hedged leg (binary risk)


@dataclass
class Market:
    name: str
    yes: str
    no: str


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", flush=True)


class Gateway:
    def __init__(self, client, markets, caps: Caps, live: bool, skew: bool = True,
                 fill: "FillPolicy" = None, arm_exits: bool = False):
        self.c = client
        self.markets = markets
        self.caps = caps
        self.live = live
        self.skew = skew            # don't add to a side we already hold inventory in
        self.fill = fill or FillPolicy()
        self.arm_exits = arm_exits  # 2nd gate: actually PLACE exit (SELL) orders
        self.last_md_ok = time.time()
        self.last_mid = {}          # token_id -> last mid we quoted around
        self._flattened = False

    def positions(self):
        """Inventory we hold, keyed by asset_id: {size, avg, end (ISO), cond}. data-api."""
        try:
            url = f"https://data-api.polymarket.com/positions?user={DEPOSIT_WALLET}"
            req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
            data = json.loads(urllib.request.urlopen(req, timeout=8).read())
            out = {}
            for p in (data or []):
                sz = float(p.get("size", 0) or 0)
                if sz <= 0.5:
                    continue
                out[p.get("asset")] = {"size": sz, "avg": float(p.get("avgPrice") or 0),
                                       "end": p.get("endDate"), "cond": p.get("conditionId")}
            return out
        except Exception:
            return {}

    def bbo(self, token):
        """Best bid / best ask for a token (where we could SELL / BUY). (None,None) on err."""
        try:
            b = self.c.get_order_book(token)
            bids = b["bids"] if isinstance(b, dict) else b.bids
            asks = b["asks"] if isinstance(b, dict) else b.asks
            g = lambda x: float(x["price"]) if isinstance(x, dict) else float(x.price)
            bb = max((g(x) for x in bids), default=None)
            ba = min((g(x) for x in asks), default=None)
            return bb, ba
        except Exception:
            return None, None

    @staticmethod
    def dte_of(pos):
        end = pos.get("end")
        if not end:
            return None
        try:
            import datetime as _dt
            e = _dt.datetime.fromisoformat(str(end).replace("Z", "+00:00"))
            return (e - _dt.datetime.now(_dt.timezone.utc)).days
        except Exception:
            return None

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

    # ---- quote / exit builders ------------------------------------------
    def _quote_for(self, token):
        """Post-only BUY quote dict for a token, or None on md error / too-low bid."""
        mid = self.mid(token)
        tick = float(self.c.get_tick_size(token))
        bid = self.round_tick(mid - self.caps.half_spread, tick)
        if bid < tick:
            return None
        return {"token": token, "side": "BUY", "price": bid, "size": self.caps.size,
                "tick": tick, "kind": "quote", "reason": "two-sided"}

    def _exit_for(self, token, pos):
        """Decide whether to flatten a legged inventory token. Returns a marketable-SELL
        exit dict (kind='exit'), or None to HOLD the leg (complete-the-hedge / cushion).

        State machine, in priority order:
          DTE     — close to resolution → flatten (don't carry binary risk)
          STOP    — bid ≤ fill − stop_loss → cut the loss
          SCRATCH — bid ≥ fill + scratch_target → take the favourable exit, go flat
          (else)  — HOLD: stay in the [fill−stop, fill+scratch] band, keep the hedge alive
        """
        fp = self.fill
        bid, _ = self.bbo(token)
        if bid is None:
            return None
        tick = float(self.c.get_tick_size(token))
        fill = float(pos.get("avg") or 0)
        size = float(pos.get("size") or 0)
        dte = self.dte_of(pos)
        markout = bid - fill
        px = self.round_tick(bid, tick)                 # sell into the touch (FAK)
        if dte is not None and dte <= fp.dte_flatten_days:
            why = f"DTE {dte}d ≤ {fp.dte_flatten_days}: flatten leg (bid {bid:.2f} / fill {fill:.2f})"
        elif markout <= -fp.stop_loss:
            why = f"STOP: bid {bid:.2f} ≤ fill {fill:.2f}−{fp.stop_loss:.2f} (markout {markout:+.2f})"
        elif markout >= fp.scratch_target:
            why = f"SCRATCH: bid {bid:.2f} ≥ fill {fill:.2f}+{fp.scratch_target:.2f} (markout {markout:+.2f})"
        else:
            return None                                  # HOLD
        return {"token": token, "side": "SELL", "price": px, "size": size,
                "tick": tick, "kind": "exit", "reason": why}

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

        pos = self.positions()         # inventory: drives skew AND fill-handling
        desired = []                   # list of order dicts (kind: quote | exit)
        for mk in self.markets:
            hy = (pos.get(mk.yes) or {}).get("size", 0.0)
            hn = (pos.get(mk.no) or {}).get("size", 0.0)
            # A complete (hedged) set is risk-free locked profit — the MM never touches it.
            if hy > 0.5 and hn > 0.5:
                log(f"{mk.name}: hedged set held (YES {hy:.0f} / NO {hn:.0f}) — no action")
                continue
            # which side, if any, is the legged (single-sided) inventory
            leg = (mk.yes, pos[mk.yes], mk.no) if hy > 0.5 else \
                  (mk.no, pos[mk.no], mk.yes) if hn > 0.5 else None
            try:
                if leg is None:                                  # flat → quote both sides
                    for token in (mk.yes, mk.no):
                        q = self._quote_for(token)
                        if q: desired.append(q)
                else:
                    leg_token, leg_pos, other = leg
                    ex = self._exit_for(leg_token, leg_pos) if self.fill.enabled else None
                    if ex is not None:                           # flatten the leg, pause the hedge
                        desired.append(ex)
                        log(f"{mk.name}: {ex['reason']} → flatten leg, pause other-side quote")
                    else:                                        # hold leg, complete the hedge
                        log(f"skew: hold {leg_pos['size']:.0f} tok…{leg_token[-6:]} (leg) — "
                            f"quoting other side to complete hedge")
                        q = self._quote_for(other)
                        if q: desired.append(q)
                        if not self.skew:                        # --no-skew → also re-quote the leg
                            q2 = self._quote_for(leg_token)
                            if q2: desired.append(q2)
                self.last_md_ok = time.time()
            except Exception as e:
                log(f"md error {mk.name}: {e!r}")
                continue

        buys = [d for d in desired if d["side"] == "BUY"]
        exits = [d for d in desired if d["side"] == "SELL"]

        # 3) caps apply to BUY exposure only — exits REDUCE risk and must never be blocked
        if any(d["price"] * d["size"] > caps.max_order_notional for d in buys):
            log("SKIP buys: an order exceeds max_order_notional"); buys = []
        buy_notional = sum(d["price"] * d["size"] for d in buys)
        if buy_notional > caps.max_total_notional:
            log(f"SKIP buys: total ${buy_notional:.2f} > cap ${caps.max_total_notional}"); buys = []
        if len(buys) > caps.max_open_orders:
            log(f"SKIP buys: {len(buys)} > max_open_orders"); buys = []
        place = exits + buys

        # 4) re-quote only if needed: a fresh exit, nothing live, or a buy moved enough
        need = (not live_orders) or bool(exits)
        for d in buys:
            prev = self.last_mid.get(d["token"])
            if prev is None or abs(d["price"] - prev) >= caps.requote_move:
                need = True
        if not need:
            log(f"hold: {len(live_orders)} live, ${total_notional:.2f} notional")
            return

        # 5) replace: cancel resting orders, then place exits (FAK) + fresh quotes (GTD)
        log(f"re-quote: cancel {len(live_orders)}, place {len(exits)} exit / {len(buys)} quote "
            f"(${buy_notional:.2f} buy notional)")
        if self.live and live_orders:
            try: self.c.cancel_all()
            except Exception as e: log(f"cancel_all ERROR {e!r}")

        for d in place:
            token, side, price, size, kind = d["token"], d["side"], d["price"], d["size"], d["kind"]
            tag = f"{side} {size}@{price:.2f} tok…{token[-6:]} [{kind}]"
            if not self.live:
                log(f"  DRY  {tag}  ({d['reason']})"); continue
            if side == "SELL" and not self.arm_exits:        # 2nd gate on real liquidation
                log(f"  (exit not armed — would: {tag})  ({d['reason']})"); continue
            try:
                neg = self.c.get_neg_risk(token)
                ts = self.c.get_tick_size(token)
                if kind == "exit":
                    # marketable: fill what we can at the bid now, kill the rest (no resting)
                    resp = self.c.create_and_post_order(
                        OrderArgsV2(token_id=token, price=price, size=size, side=side),
                        options=PartialCreateOrderOptions(tick_size=ts, neg_risk=neg),
                        order_type=OrderType.FAK, post_only=False)
                else:
                    # GTD: auto-expires after gtd_expiry s if the process/box dies →
                    # resting quotes can never sit naked overnight again.
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
            if side == "BUY":
                self.last_mid[token] = price

        self.write_status(live_orders, buy_notional)

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
    # automated fill-handling (legged-fill state machine). OFF by default; exits double-gated.
    ap.add_argument("--fill-handling", action="store_true",
                    help="enable scratch/stop/DTE-flatten logic on legged fills (default OFF)")
    ap.add_argument("--arm-exits", action="store_true",
                    help="actually PLACE exit (SELL) orders — 2nd gate; needs --live + --fill-handling")
    ap.add_argument("--scratch-target", type=float, default=0.10,
                    help="bid ≥ fill+this → take the favourable exit, go flat")
    ap.add_argument("--stop-loss", type=float, default=0.12,
                    help="bid ≤ fill−this → cut the loss, go flat")
    ap.add_argument("--dte-flatten-days", type=int, default=1,
                    help="flatten any un-hedged leg when days-to-resolution ≤ this")
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

    fill = FillPolicy(enabled=a.fill_handling, scratch_target=a.scratch_target,
                      stop_loss=a.stop_loss, dte_flatten_days=a.dte_flatten_days)
    gw = Gateway(client, markets, caps, a.live, skew=not a.no_skew,
                 fill=fill, arm_exits=a.arm_exits)
    log(f"inventory skew: {'ON' if gw.skew else 'OFF'}")
    if fill.enabled:
        log(f"fill-handling: ON (scratch≥fill+{fill.scratch_target} · stop≤fill−{fill.stop_loss} · "
            f"DTE≤{fill.dte_flatten_days}d) · exits {'ARMED' if a.arm_exits else 'NOT armed (log-only)'}")
    else:
        log("fill-handling: OFF (plain skew — hold leg, quote other side)")
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
