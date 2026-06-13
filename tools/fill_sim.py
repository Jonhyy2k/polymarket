#!/usr/bin/env python3
"""Shadow fill simulator for the liquidity-rewards maker strategy.

Connects to the live CLOB market feed for reward-active markets, replays the book
in real time, posts the strategy's two-sided quotes (1 tick inside mid, size =
min_size), and:
  - tracks queue position and detects FILLS from last_trade_price events,
  - marks ADVERSE SELECTION: mid at fill_time + HOLD_S vs fill price,
  - estimates the gross REWARD share from live qualifying book depth,
giving a rough NET = reward + inventory mark-to-market. Zero risk: no orders sent.

This is an estimator, not a backtest of record — fills are modeled from public
trades + queue position, which is approximate (cancels vs fills are not
distinguishable on the public feed). Treat outputs as order-of-magnitude.
"""
import asyncio, json, time, sys, urllib.request, math
from collections import defaultdict
import websockets

WS = "wss://ws-subscriptions-clob.polymarket.com/ws/market"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 240.0
CONFIG = sys.argv[2] if len(sys.argv) > 2 else "config.rewards.json"
# --any: simulate fills on ALL markets even without active rewards (model test).
FORCE_ANY = "--any" in sys.argv
HOLD_S = 30.0          # adverse-selection mark horizon after a fill

def gget(url):
    req = urllib.request.Request(url, headers={"User-Agent": "pm-arb/1.0"})
    return json.load(urllib.request.urlopen(req, timeout=25))

def main():
    cfg = json.load(open(CONFIG))
    markets = {}   # token_id -> market meta
    assets = []
    for c in cfg["contracts"]:
        try:
            m = gget(f"https://clob.polymarket.com/markets/{c['condition_id']}")
        except Exception:
            continue
        r = m.get("rewards") or {}
        if not r.get("rates") and not FORCE_ANY:
            continue
        pool = sum(float(x["rewards_daily_rate"]) for x in (r.get("rates") or []))
        tick = float(m.get("minimum_tick_size") or 0.01)
        v = float(r["max_spread"]) if r.get("max_spread") else 3.0   # cents
        minsz = float(r["min_size"]) if r.get("min_size") else float(m.get("minimum_order_size") or 5)
        for tok in m.get("tokens", []):
            tid = str(tok["token_id"])
            markets[tid] = {"name": c["name"][:34], "pool": pool, "tick": tick,
                            "v": v, "minsz": minsz, "cid": c["condition_id"]}
            assets.append(tid)
    print(f"simulating {len(markets)} reward tokens for {DURATION:.0f}s, hold={HOLD_S:.0f}s")

    books = defaultdict(lambda: {"bids": {}, "asks": {}})  # token -> price->size
    mids = defaultdict(list)        # token -> [(t, mid)]
    quotes = {}                     # token -> {"bid":(px,sz,queue_ahead), "ask":(...)}
    fills = []                      # (t, token, side, px, size)
    trade_count = [0]               # last_trade_price events seen on our tokens
    qmin_acc = defaultdict(float)   # token -> sum of per-update est Qmin
    qmin_n = defaultdict(int)

    def best(book):
        if not book["bids"] or not book["asks"]:
            return None, None
        return max(book["bids"]), min(book["asks"])

    def snap(x, tick, up):
        n = x / tick
        return round((math.ceil(n) if up else math.floor(n)) * tick, 6)

    def requote(tid, t):
        m = markets[tid]; bk = books[tid]
        bb, ba = best(bk)
        if bb is None or ba <= bb:
            quotes.pop(tid, None); return
        mid = (bb + ba) / 2.0
        mids[tid].append((t, mid))
        tick = m["tick"]
        bid_px = snap(mid - tick, tick, False)
        ask_px = snap(mid + tick, tick, True)
        if bid_px >= ba: bid_px = round(ba - tick, 6)
        if ask_px <= bb: ask_px = round(bb + tick, 6)
        if bid_px <= 0 or ask_px >= 1 or ask_px <= bid_px:
            quotes.pop(tid, None); return
        # eligibility within max_spread (cents)
        if abs(bid_px - mid) * 100 >= m["v"] or abs(ask_px - mid) * 100 >= m["v"]:
            quotes.pop(tid, None); return
        sz = m["minsz"]
        # queue ahead = existing size resting at our price level
        q_bid = bk["bids"].get(bid_px, 0.0)
        q_ask = bk["asks"].get(ask_px, 0.0)
        prev = quotes.get(tid)
        # preserve queue if price unchanged
        if prev and prev["bid"][0] == bid_px:
            q_bid = prev["bid"][2]
        if prev and prev["ask"][0] == ask_px:
            q_ask = prev["ask"][2]
        quotes[tid] = {"bid": (bid_px, sz, q_bid), "ask": (ask_px, sz, q_ask)}
        # reward score (quadratic) for telemetry
        v = m["v"]
        s = tick * 100
        S = ((v - s) / v) ** 2 if s < v else 0
        qmin_acc[tid] += S * sz   # balanced two-sided central ~ one side
        qmin_n[tid] += 1

    def on_trade(tid, t, price, side, size):
        q = quotes.get(tid)
        if not q:
            return
        # A resting maker order fills only when a trade prints AT its price level
        # (after the queue ahead is consumed). A SELL printing below our bid, or a
        # BUY above our ask, does NOT touch us — using <=/>= would massively
        # over-count fills (and adverse selection). Prices are grid-aligned.
        EPS = 1e-6
        if side == "SELL" and abs(price - q["bid"][0]) < EPS:
            px, sz, qa = q["bid"]
            consume = max(0.0, size - qa)            # fills after queue ahead
            newqa = max(0.0, qa - size)
            filled = min(sz, consume)
            if filled > 0:
                fills.append((t, tid, "BUY", px, filled))   # we bought (long)
                q["bid"] = (px, sz - filled, 0.0)
            else:
                q["bid"] = (px, sz, newqa)
        elif side == "BUY" and abs(price - q["ask"][0]) < EPS:
            px, sz, qa = q["ask"]
            consume = max(0.0, size - qa)
            newqa = max(0.0, qa - size)
            filled = min(sz, consume)
            if filled > 0:
                fills.append((t, tid, "SELL", px, filled))  # we sold (short)
                q["ask"] = (px, sz - filled, 0.0)
            else:
                q["ask"] = (px, sz, newqa)

    async def run():
        async with websockets.connect(WS, max_size=None, ping_interval=10) as ws:
            await ws.send(json.dumps({"assets_ids": assets, "type": "market",
                                      "initial_dump": True, "custom_feature_enabled": True}))
            t_end = time.time() + DURATION
            while time.time() < t_end:
                try:
                    msg = await asyncio.wait_for(ws.recv(), timeout=max(0.1, t_end - time.time()))
                except asyncio.TimeoutError:
                    break
                now = time.time()
                doc = json.loads(msg); evs = doc if isinstance(doc, list) else [doc]
                for ev in evs:
                    et = ev.get("event_type"); tid = ev.get("asset_id")
                    if et == "book" and tid in markets:
                        bk = books[tid]; bk["bids"].clear(); bk["asks"].clear()
                        for b in ev.get("bids", []):
                            bk["bids"][round(float(b["price"]), 6)] = float(b["size"])
                        for a in ev.get("asks", []):
                            bk["asks"][round(float(a["price"]), 6)] = float(a["size"])
                        requote(tid, now)
                    elif et == "price_change":
                        for ch in ev.get("price_changes", []):
                            t2 = ch.get("asset_id")
                            if t2 not in markets: continue
                            bk = books[t2]
                            p = round(float(ch["price"]), 6); s = float(ch["size"])
                            side = "bids" if ch.get("side") == "BUY" else "asks"
                            if s == 0: bk[side].pop(p, None)
                            else: bk[side][p] = s
                            requote(t2, now)
                    elif et == "last_trade_price" and tid in markets:
                        trade_count[0] += 1
                        on_trade(tid, now, float(ev["price"]), ev.get("side"), float(ev["size"]))
    asyncio.run(run())

    # ---- adverse selection: mark each fill at fill_time + HOLD_S ----
    def mid_at(tid, t):
        series = mids[tid]
        chosen = None
        for (tt, mm) in series:
            if tt <= t: chosen = mm
            else: break
        return chosen

    inv_pnl = 0.0; marked = 0; per_mkt = defaultdict(lambda: [0, 0.0])
    for (t, tid, side, px, sz) in fills:
        fut = mid_at(tid, t + HOLD_S)
        if fut is None:    # not enough future data to mark
            continue
        # long (we BUY): pnl = (future_mid - px); short (we SELL): (px - future_mid)
        pnl = (fut - px) * sz if side == "BUY" else (px - fut) * sz
        inv_pnl += pnl; marked += 1
        per_mkt[tid][0] += 1; per_mkt[tid][1] += pnl

    # ---- gross reward estimate from live qualifying depth (snapshot) ----
    def field_qmin(tid):
        m = markets[tid]; bk = books[tid]; bb, ba = best(bk)
        if bb is None: return 0.0
        mid = (bb + ba) / 2.0; v = m["v"]
        def side_q(d):
            tot = 0.0
            for p, s in d.items():
                sc = abs(p - mid) * 100
                if sc < v: tot += ((v - sc) / v) ** 2 * s
            return tot
        return min(side_q(bk["bids"]), side_q(bk["asks"]))

    frac = DURATION / 86400.0
    gross_reward = 0.0
    # collapse YES/NO of same market: reward is per token-market
    seen = set()
    for tid, m in markets.items():
        if qmin_n[tid] == 0: continue
        myq = qmin_acc[tid] / qmin_n[tid]
        fq = field_qmin(tid)
        share = myq / (fq + myq) if (fq + myq) > 0 else 0
        gross_reward += m["pool"] * share * frac   # reward earned during the run window

    print(f"\n==== FILL SIMULATION ({DURATION:.0f}s window) ====")
    print(f"trades seen on our tokens: {trade_count[0]}")
    print(f"fills: {len(fills)}  marked(@+{HOLD_S:.0f}s): {marked}")
    print(f"gross reward (this window): ${gross_reward:.4f}  -> ${gross_reward/frac:.2f}/day extrapolated")
    print(f"inventory mark-to-market (adverse selection): ${inv_pnl:.4f}")
    net = gross_reward + inv_pnl
    print(f"NET (this window): ${net:.4f}  -> ${net/frac:.2f}/day extrapolated")
    if marked:
        print("\nper-market fills / inventory PnL:")
        for tid, (n, pnl) in sorted(per_mkt.items(), key=lambda x: x[1][1]):
            print(f"  {markets[tid]['name']:34} fills={n:3d}  inv_pnl=${pnl:+.4f}")
    print("\nNOTE: fills modeled from public trades + queue position (cancels not "
          "observable), reward share approximated from aggregate book depth, and a "
          "short window has few trades. Order-of-magnitude only.")

if __name__ == "__main__":
    main()
