#!/usr/bin/env python3
"""Paper test: buy a position + complete it with the opposite (complement) side,
and check whether that round-trip is ever a FREE GAIN (real arbitrage).

For each current market it pulls live YES and NO books and shows:
  1. the REAL per-token mids (NOT 0.50) and that yes_mid + no_mid ≈ $1.00,
  2. BUY-BOTH (taker): pay yes_ask + no_ask, redeem $1 at resolution
        edge = $1.00 - (yes_ask + no_ask)   > 0 ⇒ free money
  3. SELL-BOTH (taker): mint a pair for $1, sell yes_bid + no_bid
        edge = (yes_bid + no_bid) - $1.00   > 0 ⇒ free money
  4. MID fake-buy + hedge (the maker dream): buy YES at its mid (paper fill),
        hedge by buying NO at its ask → locked = $1.00 - (yes_mid + no_ask)

Expectation on efficient books: no free gain (you PAY the spread as a taker; the
mid round-trip only "works" if BOTH legs fill at mid, which is the maker game and
adverse selection prevents). This makes that concrete on live data.

Usage: python3 tools/hedge_arb_test.py [config.json] [fee_bps]
  no config  -> pulls fresh liquid two-sided crypto markets from gamma
  fee_bps    -> taker fee per leg in bps applied to net columns (default 0 = raw)
"""
import json, sys, urllib.request, math

UA = {"User-Agent": "pm-arb/1.0"}
def gget(url):
    return json.load(urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=25))

def fresh_crypto_markets(n=18):
    url = ("https://gamma-api.polymarket.com/markets?closed=false&active=true&limit=200"
           "&tag_id=21&related_tags=true&order=volume24hr&ascending=false")
    out = []
    seen = set()
    for m in gget(url):
        if not (m.get("acceptingOrders") and m.get("enableOrderBook")) or m.get("closed"): continue
        bid, ask = m.get("bestBid"), m.get("bestAsk")
        if bid is None or ask is None: continue
        if not (0.05 <= bid and ask <= 0.95): continue        # two-sided, away from extremes
        toks = m.get("clobTokenIds")
        if not toks: continue
        try: t = json.loads(toks)
        except: continue
        if len(t) != 2 or m["conditionId"] in seen: continue
        seen.add(m["conditionId"])
        out.append({"name": (m.get("question") or "")[:40], "yes": str(t[0]), "no": str(t[1])})
        if len(out) >= n: break
    return out

def book_bbo(token_id):
    bk = gget(f"https://clob.polymarket.com/book?token_id={token_id}")
    bids = bk.get("bids") or []; asks = bk.get("asks") or []
    if not bids or not asks: return None
    bb = max(float(b["price"]) for b in bids)
    ba = min(float(a["price"]) for a in asks)
    return bb, ba

def main():
    cfgpath = sys.argv[1] if len(sys.argv) > 1 and sys.argv[1].endswith(".json") else None
    fee_bps = float(sys.argv[2]) if len(sys.argv) > 2 else (float(sys.argv[1]) if (len(sys.argv) > 1 and not cfgpath) else 0.0)
    if cfgpath:
        mks = [{"name": c["name"][:40], "yes": c["token_id_yes"], "no": c["token_id_no"]}
               for c in json.load(open(cfgpath))["contracts"]]
    else:
        mks = fresh_crypto_markets()
    fee = fee_bps / 10000.0

    print(f"{'market':42}{'Ymid':>6}{'Nmid':>6}{'sumMid':>7}{'buyBoth':>8}{'edgeBuy¢':>9}{'edgeSell¢':>10}{'midHedge¢':>10}")
    n_buy_arb = n_sell_arb = 0
    edges = []
    for m in mks:
        y = book_bbo(m["yes"]); no = book_bbo(m["no"])
        if not y or not no: continue
        yb, ya = y; nb, na = no
        ymid = (yb + ya) / 2; nmid = (nb + na) / 2
        # taker round-trips (fees subtracted from the gain)
        buy_cost = ya + na
        edge_buy = (1.0 - buy_cost) - fee * buy_cost           # >0 = free money
        sell_proc = yb + nb
        edge_sell = (sell_proc - 1.0) - fee * sell_proc        # >0 = free money
        # mid fake-buy on YES, hedge by taking NO ask (realizable hedge)
        mid_hedge = 1.0 - (ymid + na)
        edges.append(edge_buy)
        if edge_buy > 0: n_buy_arb += 1
        if edge_sell > 0: n_sell_arb += 1
        flag = "  <-- FREE BUY ARB" if edge_buy > 0 else ("  <-- FREE SELL ARB" if edge_sell > 0 else "")
        print(f"{m['name']:42}{ymid:6.3f}{nmid:6.3f}{ymid+nmid:7.3f}{buy_cost:8.3f}"
              f"{edge_buy*100:9.2f}{edge_sell*100:10.2f}{mid_hedge*100:10.2f}{flag}")

    print(f"\n--- {len(edges)} markets tested (fee={fee_bps:.0f} bps/leg) ---")
    print(f"markets with a FREE buy-both arb : {n_buy_arb}")
    print(f"markets with a FREE sell-both arb: {n_sell_arb}")
    if edges:
        edges.sort()
        print(f"buy-both edge (¢): best={edges[-1]*100:+.2f}  median={edges[len(edges)//2]*100:+.2f}  worst={edges[0]*100:+.2f}")
    print("\nReading it: sumMid≈1.000 confirms YES+NO=$1 (mids are NOT 0.50). 'edgeBuy¢'/'edgeSell¢'")
    print("are the GUARANTEED cents you'd gain per share completing both sides as a TAKER — negative")
    print("means you pay the spread (no free lunch). 'midHedge¢' is the maker dream (fill YES at mid,")
    print("hedge NO at ask): if it's >0 you'd profit IF your YES rests and fills at mid — which is the")
    print("liquidity-maker game, and adverse selection is the catch.")

if __name__ == "__main__":
    main()
