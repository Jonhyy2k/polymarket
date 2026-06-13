#!/usr/bin/env python3
"""Phase 1: build a live config from current gamma crypto markets.

Selects active, order-book-enabled crypto markets that are still open, preferring
short-dated markets with genuine two-sided books (tighter spreads = real signal),
and writes a config.json carrying over the strategy settings.
"""
import json, sys, datetime, urllib.request, re

GAMMA = "https://gamma-api.polymarket.com/markets"
STRATEGY = "config.strategy.json"
OUT = sys.argv[1] if len(sys.argv) > 1 else "config.live.json"
MAX_MARKETS = int(sys.argv[2]) if len(sys.argv) > 2 else 16

CRYPTO_RE = re.compile(r"(?i)bitcoin|ethereum|\bbtc\b|\beth\b|\bsol\b|solana|\bxrp\b|dogecoin|crypto")

def fetch(params):
    url = GAMMA + "?" + "&".join(f"{k}={v}" for k, v in params.items())
    req = urllib.request.Request(url, headers={"User-Agent": "polymarket-arb/1.0"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.load(r)

def main():
    now = datetime.datetime.now(datetime.timezone.utc).isoformat().replace("+00:00", "Z")
    markets = {}
    # tag_id=21 is crypto; pull by both volume and liquidity orderings to widen the net
    for order in ("volume24hr", "liquidityClob"):
        for page in fetch({"closed": "false", "active": "true", "limit": 200,
                           "tag_id": 21, "related_tags": "true",
                           "order": order, "ascending": "false"}):
            cid = page.get("conditionId")
            if cid:
                markets[cid] = page

    cands = []
    for m in markets.values():
        if m.get("closed") or not m.get("active"): continue
        if not m.get("acceptingOrders") or not m.get("enableOrderBook"): continue
        q = m.get("question") or ""
        if not CRYPTO_RE.search(q): continue
        end = m.get("endDate") or ""
        if end and end < now: continue
        toks = m.get("clobTokenIds")
        if not toks: continue
        try:
            tok = json.loads(toks)
        except Exception:
            continue
        if len(tok) != 2: continue
        bid = m.get("bestBid"); ask = m.get("bestAsk")
        two_sided = bid is not None and ask is not None and 0.02 <= bid and ask <= 0.98
        cands.append({
            "name": q[:60], "condition_id": cid,
            "token_id_yes": str(tok[0]), "token_id_no": str(tok[1]),
            "end": end, "liq": m.get("liquidityClob") or 0,
            "tick": m.get("orderPriceMinTickSize"),
            "two_sided": two_sided, "bid": bid, "ask": ask,
        })

    # Prefer: two-sided first, then soonest expiry, then highest liquidity.
    cands.sort(key=lambda c: (not c["two_sided"], c["end"], -c["liq"]))
    chosen = cands[:MAX_MARKETS]

    cfg = json.load(open(STRATEGY))
    cfg["log_file"] = "logs/arb_live.csv"
    cfg["near_miss_log_file"] = "logs/near_miss_live.csv"
    cfg["replay_log_file"] = "logs/replay_live.csv"
    cfg["contracts"] = [{"name": c["name"], "condition_id": c["condition_id"],
                         "token_id_yes": c["token_id_yes"], "token_id_no": c["token_id_no"]}
                        for c in chosen]
    json.dump(cfg, open(OUT, "w"), indent=4)

    print(f"{len(cands)} candidates, wrote {len(chosen)} to {OUT}")
    for c in chosen:
        flag = "2SIDE" if c["two_sided"] else "  ext"
        b = f"{c['bid']:.3f}" if c['bid'] is not None else "  -  "
        a = f"{c['ask']:.3f}" if c['ask'] is not None else "  -  "
        print(f"  [{flag}] tick={c['tick']} {b}/{a} end={c['end']} | {c['name']}")

if __name__ == "__main__":
    main()
