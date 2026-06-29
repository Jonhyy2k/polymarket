#!/usr/bin/env python3
"""
Fill telemetry — measures ADVERSE SELECTION, the go/no-go number for the LP strategy.

For every fill on the deposit wallet it records the fill, then samples the market
midpoint at +1m / +5m / +15m and computes the mark-out (mid_T - fill, signed by side):
  > 0  the price moved IN our favour after the fill  (good)
  < 0  the price moved AGAINST us after the fill      = adverse selection (the cost)

A two-sided LP earns rewards but pays this mark-out on every fill. If, across many
fills, rewards > |adverse mark-out|, the strategy makes money; if not, it bleeds.
This tool produces the dataset to decide that — run it alongside any live quoting.

Output:  logs/fill_telemetry.csv   (one row per fill, written once its 15m mark-out lands)
State:   logs/fill_telemetry_state.json  (resumes cleanly across restarts)

Run continuously (read-only, no orders ever placed):
    PYTHONPATH=./.pmlibs python3 tools/fill_telemetry.py            # daemon
    PYTHONPATH=./.pmlibs python3 tools/fill_telemetry.py --once     # one poll then exit (test)
"""
import argparse, csv, json, os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".pmlibs"))
from py_clob_client_v2 import ClobClient, ApiCreds  # noqa: E402

HOST = "https://clob.polymarket.com"
DEP = "0x832317706479bb6762741B9b9ba568bb86fFfFF0"
CREDS = "/home/ubuntu/.pm_creds.env"
KEYF = "/home/ubuntu/.pm_signer_key"
BASE = os.path.join(os.path.dirname(__file__), "..")
CSV_PATH = os.path.join(BASE, "logs", "fill_telemetry.csv")
STATE_PATH = os.path.join(BASE, "logs", "fill_telemetry_state.json")
HORIZONS = [60, 300, 900]   # seconds: 1m / 5m / 15m mark-outs
POLL = 20

CSV_COLS = ["fill_ts", "role", "side", "outcome", "fill_price", "size", "asset", "market",
            "mid_0"] + [f"mid_{h}" for h in HORIZONS] + [f"markout_{h}" for h in HORIZONS]


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


def log(m):
    print(f"[{time.strftime('%H:%M:%S')}] {m}", flush=True)


def mk_client():
    e = read_env(CREDS)
    return ClobClient(host=HOST, chain_id=137, key=open(KEYF).read().strip(),
                      creds=ApiCreds(e["PM_API_KEY"], e["PM_API_SECRET"], e["PM_API_PASSPHRASE"]),
                      signature_type=3, funder=DEP)


def midpoint(c, asset):
    try:
        m = c.get_midpoint(asset)
        return float(m["mid"]) if isinstance(m, dict) else float(m)
    except Exception:
        return None


def load_state():
    try:
        s = json.load(open(STATE_PATH))
        return set(s.get("seen", [])), s.get("pending", [])
    except Exception:
        return set(), []


def save_state(seen, pending):
    tmp = STATE_PATH + ".tmp"
    json.dump({"seen": sorted(seen), "pending": pending}, open(tmp, "w"))
    os.replace(tmp, STATE_PATH)


def our_fills(trades):
    """Extract this wallet's fills from get_trades() (maker legs + our taker orders)."""
    out = []
    for t in trades or []:
        tid = t.get("id", "")
        if t.get("trader_side") == "TAKER":
            out.append({"key": f"{tid}:taker", "role": "taker", "asset": t.get("asset_id"),
                        "side": t.get("side"), "outcome": t.get("outcome"),
                        "price": float(t.get("price") or 0), "size": float(t.get("size") or 0),
                        "ts": int(t.get("match_time") or time.time()), "market": t.get("market")})
        for m in t.get("maker_orders", []):
            if (m.get("maker_address") or "").lower() == DEP.lower():
                out.append({"key": f"{tid}:{(m.get('order_id') or '')[:10]}", "role": "maker",
                            "asset": m.get("asset_id"), "side": m.get("side"),
                            "outcome": m.get("outcome"), "price": float(m.get("price") or 0),
                            "size": float(m.get("matched_amount") or 0),
                            "ts": int(t.get("match_time") or time.time()), "market": t.get("market")})
    return out


def markout(side, fill, mid):
    if mid is None:
        return None
    return round((mid - fill) if side == "BUY" else (fill - mid), 4)


def write_row(rec):
    new = not os.path.exists(CSV_PATH)
    with open(CSV_PATH, "a", newline="") as f:
        w = csv.DictWriter(f, fieldnames=CSV_COLS)
        if new:
            w.writeheader()
        row = {k: rec.get(k) for k in CSV_COLS}
        w.writerow(row)


def cycle(c, seen, pending):
    # 1) discover new fills
    try:
        fills = our_fills(c.get_trades())
    except Exception as e:
        log(f"get_trades error: {e!r}")
        fills = []
    now0 = time.time()
    for fobj in fills:
        if fobj["key"] in seen:
            continue
        seen.add(fobj["key"])
        if now0 - fobj["ts"] > HORIZONS[-1] + 120:   # already too old to mark out cleanly
            log(f"skip stale fill {fobj['side']} {fobj['outcome']} @{fobj['price']} (age {int(now0-fobj['ts'])}s)")
            continue
        rec = dict(fobj)
        rec["mid_0"] = midpoint(c, fobj["asset"])
        rec["samples"] = {}          # horizon -> mid
        pending.append(rec)
        log(f"NEW FILL {rec['role']} {rec['side']} {rec['outcome']} {rec['size']}@{rec['price']} "
            f"mid_0={rec['mid_0']} asset…{str(rec['asset'])[-6:]}")

    # 2) sample due mark-outs
    now = time.time()
    still = []
    for rec in pending:
        done = True
        for h in HORIZONS:
            if str(h) in rec["samples"]:
                continue
            if now >= rec["ts"] + h:
                rec["samples"][str(h)] = midpoint(c, rec["asset"])
            else:
                done = False
        if done:
            for h in HORIZONS:
                rec[f"mid_{h}"] = rec["samples"].get(str(h))
                rec[f"markout_{h}"] = markout(rec["side"], rec["price"], rec[f"mid_{h}"])
            write_row(rec)
            log(f"COMPLETE fill {rec['key'][:14]} markouts "
                + " ".join(f"{h}s={rec[f'markout_{h}']}" for h in HORIZONS))
        else:
            still.append(rec)
    return seen, still


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--once", action="store_true", help="run one poll cycle then exit")
    a = ap.parse_args()
    os.makedirs(os.path.join(BASE, "logs"), exist_ok=True)
    c = mk_client()
    seen, pending = load_state()
    log(f"fill_telemetry up · seen={len(seen)} pending={len(pending)} · CSV={CSV_PATH}")
    while True:
        seen, pending = cycle(c, seen, pending)
        save_state(seen, pending)
        if a.once:
            log("--once done"); break
        time.sleep(POLL)


if __name__ == "__main__":
    main()
