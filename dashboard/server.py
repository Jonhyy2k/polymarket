#!/usr/bin/env python3
"""
Polymarket LP Risk Dashboard — FastAPI + WebSocket backend.

Data sources:
  1. Polymarket CLOB REST API  (price polling every 5s)
  2. quote_telemetry.csv       (quote state: mid, qmin, position, eligibility)
  3. /tmp/bot_telemetry.log    (feed latency, reconnect count)

Run:
  python3 -m uvicorn dashboard.server:app --host 0.0.0.0 --port 8080
  (from /home/ubuntu/polymarket/)
"""

import asyncio, json, math, os, re, time, csv
from collections import defaultdict, deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Set
import urllib.request
import urllib.error

from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse

# ─── Paths ────────────────────────────────────────────────────────────────────
BASE_DIR   = Path(__file__).parent.parent
CFG_FILE   = BASE_DIR / "config.live.json"
TEL_FILE   = BASE_DIR / "logs/quote_telemetry.csv"
BOT_LOG    = Path("/tmp/bot_telemetry.log")
HTML_FILE  = Path(__file__).parent / "index.html"

CLOB_BASE  = "https://clob.polymarket.com"

# ─── Load config + build market registry ─────────────────────────────────────
with open(CFG_FILE) as f:
    _cfg = json.load(f)

EXEC_MODE = _cfg.get("exec_mode", "shadow")

_POOL_MAP = {
    "france": 3182, "argentina": 2273, "spain": 2045,
    "england": 1364, "portugal": 682, "fed": 300,
}

MARKETS: List[Dict] = []
TOKEN_SIDE: Dict[str, tuple] = {}  # token_id -> (market_dict, "yes"|"no")

for _c in _cfg["contracts"]:
    _name    = _c["name"]
    _display = {
        "france":   "France WC",
        "argentina":"Argentina WC",
        "spain":    "Spain WC",
        "england":  "England WC",
        "portugal": "Portugal WC",
        "fed":      "Fed Rate Hike",
    }.get(next((k for k in _POOL_MAP if k in _name.lower()), ""), _name[:20])

    _pool    = next((v for k, v in _POOL_MAP.items() if k in _name.lower()), 100)
    _neg     = "fifa" in _name.lower()
    # spread_thou (max allowed quote spread in 1/1000ths of $1):
    # WC: 45 thou = 4.5¢, measured Qmin=((45-1)/45)^2×200=186.9
    # Fed Hike: measured Qmin=65.3 from telemetry session 2026-06-25
    _qmin_k  = 186.9 if _neg else 65.3

    m = {
        "name":         _name,
        "display":      _display,
        "condition_id": _c["condition_id"],
        "yes_token":    _c["token_id_yes"],
        "no_token":     _c["token_id_no"],
        "pool":         _pool,
        "spread_thou":  45.0 if _neg else 35.0,
        "min_size":     200,
        "neg_risk":     _neg,
        "qmin_k":       round(_qmin_k, 1),
        # live REST prices (updated every 5s)
        "yes_bid": None, "yes_ask": None, "yes_mid": None,
        "no_bid":  None, "no_ask":  None, "no_mid":  None,
        # session reference (first mid seen) + hi/lo for Δ column
        "yes_open": None, "yes_hi": None, "yes_lo": None,
        # telemetry state (updated from CSV)
        "tel_yes_mid_thou": None, "tel_no_mid_thou": None,
        "yes_qmin": None, "no_qmin": None,
        "yes_net_pos": 0.0, "no_net_pos": 0.0,
        "yes_eligible": True, "no_eligible": True,
        "yes_at_risk": False, "no_at_risk": False,
        # price history for VaR  (deque of float: mid in $)
        "yes_history": deque(maxlen=500),
        "no_history":  deque(maxlen=500),
    }
    MARKETS.append(m)
    TOKEN_SIDE[_c["token_id_yes"]] = (m, "yes")
    TOKEN_SIDE[_c["token_id_no"]]  = (m, "no")

# ─── Shared state ─────────────────────────────────────────────────────────────
state = {
    "exec_mode":        EXEC_MODE,
    "bot_running":      False,
    "bot_pid":          None,
    "feed_p50_ms":      None,
    "feed_p99_ms":      None,
    "reconnect_count":  0,
    "msg_per_sec":      None,
    "bot_log_ts":       None,
    "rest_ok":          False,
    "tel_rows":         0,
    "tel_last_ts_ns":   0,
    "last_update":      None,
    "markets":          MARKETS,
    # on-chain wallet snapshot (polled from Polygon)
    "pusd":             None,
    "pol":              None,
    "wallet_ts":        None,
}

_clients: Set[WebSocket] = set()

# ─── On-chain wallet snapshot (collateral + gas) ──────────────────────────────
WALLET_ADDR = "0x4E3b143938947039b2F0b13BD1038683DE57851F"
PUSD_TOKEN  = "0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB"
POLYGON_RPC = "https://polygon-bor-rpc.publicnode.com"

def _rpc(method: str, params: list):
    body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": params}).encode()
    req = urllib.request.Request(POLYGON_RPC, data=body, method="POST",
                                 headers={"Content-Type": "application/json", "User-Agent": "Mozilla/5.0"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read()).get("result")

def _read_wallet():
    try:
        d = "0x70a08231" + "0" * 24 + WALLET_ADDR[2:].lower()
        pusd = _rpc("eth_call", [{"to": PUSD_TOKEN, "data": d}, "latest"])
        pol = _rpc("eth_getBalance", [WALLET_ADDR, "latest"])
        state["pusd"] = int(pusd, 16) / 1e6 if pusd and pusd != "0x" else None
        state["pol"] = int(pol, 16) / 1e18 if pol else None
        state["wallet_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_wallet():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_wallet)
        await asyncio.sleep(30)

# ─── REST price fetch ─────────────────────────────────────────────────────────
def _fetch_book(token_id: str) -> Optional[Dict]:
    url = f"{CLOB_BASE}/book?token_id={token_id}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "polymarket-lp-dashboard/1.0"})
        with urllib.request.urlopen(req, timeout=5) as r:
            return json.loads(r.read())
    except Exception:
        return None

def _best_bid_ask(book: Dict):
    bids = book.get("bids", [])
    asks = book.get("asks", [])
    bb = max((float(b["price"]) for b in bids), default=None)
    ba = min((float(a["price"]) for a in asks), default=None)
    return bb, ba

async def _poll_prices():
    while True:
        ok = False
        for m in MARKETS:
            for side, token in [("yes", m["yes_token"]), ("no", m["no_token"])]:
                book = await asyncio.get_event_loop().run_in_executor(None, _fetch_book, token)
                if book is None:
                    continue
                bb, ba = _best_bid_ask(book)
                m[f"{side}_bid"] = bb
                m[f"{side}_ask"] = ba
                if bb is not None and ba is not None:
                    mid = round((bb + ba) / 2, 4)
                    m[f"{side}_mid"] = mid
                    m[f"{side}_history"].append(mid)
                    ok = True
                    if side == "yes":
                        if m["yes_open"] is None:
                            m["yes_open"] = mid
                            m["yes_hi"] = mid
                            m["yes_lo"] = mid
                        else:
                            m["yes_hi"] = max(m["yes_hi"], mid)
                            m["yes_lo"] = min(m["yes_lo"], mid)
        state["rest_ok"] = ok
        await asyncio.sleep(5)

# ─── Telemetry CSV reader ─────────────────────────────────────────────────────
_TEL_HEADER = None
_tel_file_pos = 0  # byte position; start from current end (tail mode)

def _read_telemetry_tail():
    global _TEL_HEADER, _tel_file_pos
    if not TEL_FILE.exists():
        return

    with open(TEL_FILE, "rb") as f:
        if _tel_file_pos == 0:
            # First run: seek to last ~50k bytes to load recent state
            f.seek(0, 2)
            size = f.tell()
            _tel_file_pos = max(0, size - 500_000)
        f.seek(_tel_file_pos)
        chunk = f.read()
        if not chunk:
            return
        _tel_file_pos += len(chunk)

    lines = chunk.decode("utf-8", errors="ignore").splitlines()
    for line in lines:
        if not line or line.startswith("sample_ns"):
            if _TEL_HEADER is None and line.startswith("sample_ns"):
                _TEL_HEADER = line.split(",")
            continue
        parts = line.split(",")
        if len(parts) < 11:
            continue
        try:
            ts_ns     = int(parts[0])
            token     = parts[1]
            mid_thou  = float(parts[2])
            qmin      = float(parts[8])
            net_pos   = float(parts[9])
            eligible  = int(parts[15]) if len(parts) > 15 else 1
            bid_risk  = int(parts[11]) if len(parts) > 11 else 0
            ask_risk  = int(parts[12]) if len(parts) > 12 else 0
        except (ValueError, IndexError):
            continue

        if token not in TOKEN_SIDE:
            continue
        m, side = TOKEN_SIDE[token]
        m[f"tel_{side}_mid_thou"] = mid_thou
        m[f"{side}_qmin"]        = qmin
        m[f"{side}_net_pos"]     = net_pos
        m[f"{side}_eligible"]    = bool(eligible)
        m[f"{side}_at_risk"]     = bool(bid_risk or ask_risk)

        if ts_ns > state["tel_last_ts_ns"]:
            state["tel_last_ts_ns"] = ts_ns
        state["tel_rows"] += 1

async def _poll_telemetry():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_telemetry_tail)
        await asyncio.sleep(2)

# ─── Bot log parser ───────────────────────────────────────────────────────────
def _parse_bot_log():
    if not BOT_LOG.exists():
        state["bot_running"] = False
        return

    mtime = BOT_LOG.stat().st_mtime
    # If log not updated in 60s, consider bot stopped
    if time.time() - mtime > 60:
        state["bot_running"] = False
        return

    state["bot_running"] = True
    state["bot_log_ts"]  = datetime.fromtimestamp(mtime).strftime("%H:%M:%S")

    try:
        with open(BOT_LOG, "rb") as f:
            f.seek(max(0, f.seek(0, 2) - 8192) if f.seek(0, 2) > 8192 else 0)
            f.seek(max(0, f.tell() - 8192))
            tail = f.read().decode("utf-8", errors="ignore")
    except Exception:
        return

    # Extract latest SUMMARY block
    # feed delivery p50 X.Xms p99 X.Xms
    m = re.search(r"feed delivery p50 ([\d.]+)ms p99 ([\d.]+)ms", tail)
    if m:
        state["feed_p50_ms"] = float(m.group(1))
        state["feed_p99_ms"] = float(m.group(2))

    # e2e p50 Xus → convert µs to ms
    m2 = re.search(r"e2e p50 ([\d.]+)µs", tail)
    if m2:
        state["e2e_p50_us"] = float(m2.group(1))

    # reconnects
    reconnects = len(re.findall(r"reconnect", tail, re.IGNORECASE))
    state["reconnect_count"] = reconnects

    # msg/s from "NNN messages (NN.N/s)"
    m3 = re.search(r"(\d+) messages \(([\d.]+)/s\)", tail)
    if m3:
        state["msg_per_sec"] = float(m3.group(2))

    # PID: read /proc to find arb_detector
    try:
        import subprocess
        r = subprocess.run(["pgrep", "-x", "arb_detector"],
                           capture_output=True, text=True, timeout=1)
        pid = r.stdout.strip()
        state["bot_pid"] = pid if pid else None
        state["bot_running"] = bool(pid)
    except Exception:
        pass

async def _poll_bot_log():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _parse_bot_log)
        await asyncio.sleep(5)

# ─── Risk model ───────────────────────────────────────────────────────────────
def _compute_risk() -> Dict:
    risk = {
        "gross_notional":  0.0,
        "net_delta":       0.0,
        "max_loss":        0.0,
        "var_95_1day":     None,
        "var_note":        "",
        "stress_one_goal": 0.0,  # +20c (200 thou) on any single market YES
        "stress_all_out":  0.0,  # all YES → 0 (teams eliminated)
        "stress_winner":   0.0,  # one YES → 100c (team wins)
        "concentration":   [],
        "est_reward_day":  0.0,
        "reward_breakdown": [],
        "total_pool":      0.0,
    }

    for m in MARKETS:
        # Use REST mid if available, fallback to telemetry
        yes_p = m["yes_mid"] if m["yes_mid"] is not None else (
            (m["tel_yes_mid_thou"] / 1000.0) if m["tel_yes_mid_thou"] is not None else None
        )
        no_p = m["no_mid"] if m["no_mid"] is not None else (
            (m["tel_no_mid_thou"] / 1000.0) if m["tel_no_mid_thou"] is not None else None
        )

        if yes_p is None:
            yes_p = 0.15  # fallback estimate
        if no_p is None:
            no_p = 0.85

        yes_pos = m["yes_net_pos"]
        no_pos  = m["no_net_pos"]

        # In mocklive, net positions are 0 (simulated). Show "hypothetical" AUM.
        # We treat each side of a symmetric quote as $1 × min_size notional.
        quote_notional = yes_p * m["min_size"]  # cost of resting YES bid
        risk["gross_notional"] += quote_notional * 2  # both YES and NO sides

        # Delta (live mode)
        risk["net_delta"] += yes_pos * yes_p + no_pos * no_p

        # Max loss: long YES → lose yes_pos × yes_p if team eliminated (→0)
        #           long NO  → lose no_pos  × no_p  if team wins      (→0)
        risk["max_loss"] += abs(yes_pos) * yes_p + abs(no_pos) * no_p

        # Stress: single YES +20c spike (e.g., goal scored)
        # If long YES: gain; if short YES (net_pos<0): lose
        stress_20c = yes_pos * 0.20  # delta×move
        risk["stress_one_goal"] = max(risk["stress_one_goal"], abs(stress_20c))

        # Stress: all YES → 0 (all teams eliminated in group stage for us)
        risk["stress_all_out"] += yes_pos * (-yes_p)  # position × -price

        # Stress: one YES → 1.0 (team wins WC, all others lose)
        winner_pnl = yes_pos * (1.0 - yes_p)  # best case for long YES
        risk["stress_winner"] = max(risk["stress_winner"], abs(winner_pnl))

        # VaR from price history
        hist = list(m["yes_history"])
        if len(hist) >= 30:
            returns = [math.log(hist[i] / hist[i-1]) for i in range(1, len(hist))
                       if hist[i] > 0 and hist[i-1] > 0]
            if returns:
                mu = sum(returns) / len(returns)
                var = sum((r - mu) ** 2 for r in returns) / len(returns)
                sigma = math.sqrt(var)
                # Daily VaR: scale by sqrt(obs_per_day / obs_we_have)
                # Polling every 5s → 720 obs/hour → ~17280/day
                # We have len(hist) obs
                scale = math.sqrt(17280 / max(len(hist), 1))
                var_1d = 1.645 * sigma * scale * abs(yes_pos) * yes_p
                if risk["var_95_1day"] is None:
                    risk["var_95_1day"] = var_1d
                else:
                    risk["var_95_1day"] = (risk["var_95_1day"] ** 2 + var_1d ** 2) ** 0.5
        else:
            risk["var_note"] = f"<{len(hist)} REST samples; need ≥30"

        # Reward estimate
        qmin_yes = m["yes_qmin"] or m["qmin_k"]
        qmin_no  = m["no_qmin"]  or m["qmin_k"]
        # Conservative: assume we're 1% of the field Qmin total
        field_mult = 100.0
        share_yes = qmin_yes / (qmin_yes * field_mult + qmin_yes)
        share_no  = qmin_no  / (qmin_no  * field_mult + qmin_no)
        # Both sides share the same pool (pool split between YES and NO)
        est_reward = (share_yes + share_no) / 2 * m["pool"]
        risk["est_reward_day"] += est_reward
        risk["total_pool"]     += m["pool"]
        risk["reward_breakdown"].append({
            "display":   m["display"],
            "pool":      m["pool"],
            "qmin":      round(qmin_yes, 1),
            "est_reward": round(est_reward, 2),
        })

        # Concentration
        risk["concentration"].append({
            "display": m["display"],
            "notional": round(quote_notional * 2, 2),
        })

    risk["gross_notional"] = round(risk["gross_notional"], 2)
    risk["net_delta"]      = round(risk["net_delta"],      2)
    risk["max_loss"]       = round(risk["max_loss"],       2)
    risk["stress_all_out"] = round(risk["stress_all_out"], 2)
    risk["stress_winner"]  = round(risk["stress_winner"],  2)
    risk["est_reward_day"] = round(risk["est_reward_day"], 2)
    risk["total_pool"]     = round(risk["total_pool"],     0)
    if risk["var_95_1day"] is not None:
        risk["var_95_1day"] = round(risk["var_95_1day"], 2)

    total_notional = risk["gross_notional"] or 1
    for c in risk["concentration"]:
        c["pct"] = round(c["notional"] / total_notional * 100, 1)

    return risk

# ─── Broadcast loop ───────────────────────────────────────────────────────────
async def _broadcast_loop():
    global _clients
    while True:
        if _clients:
            payload = _build_payload()
            msg = json.dumps(payload)
            dead = set()
            for ws in _clients:
                try:
                    await ws.send_text(msg)
                except Exception:
                    dead.add(ws)
            _clients -= dead
        state["last_update"] = datetime.now(timezone.utc).isoformat()
        await asyncio.sleep(2)

def _build_payload() -> Dict:
    markets_out = []
    for m in MARKETS:
        yes_p = m["yes_mid"] or (
            m["tel_yes_mid_thou"] / 1000 if m["tel_yes_mid_thou"] else None
        )
        no_p = m["no_mid"] or (
            m["tel_no_mid_thou"] / 1000 if m["tel_no_mid_thou"] else None
        )
        yes_chg = (yes_p - m["yes_open"]) if (yes_p is not None and m["yes_open"]) else None
        markets_out.append({
            "display":     m["display"],
            "yes_bid":     m["yes_bid"],
            "yes_ask":     m["yes_ask"],
            "yes_mid":     yes_p,
            "yes_open":    m["yes_open"],
            "yes_hi":      m["yes_hi"],
            "yes_lo":      m["yes_lo"],
            "yes_chg":     round(yes_chg, 4) if yes_chg is not None else None,
            "no_bid":      m["no_bid"],
            "no_ask":      m["no_ask"],
            "no_mid":      no_p,
            "yes_net_pos": m["yes_net_pos"],
            "no_net_pos":  m["no_net_pos"],
            "yes_qmin":    m["yes_qmin"] or m["qmin_k"],
            "no_qmin":     m["no_qmin"]  or m["qmin_k"],
            "yes_eligible":m["yes_eligible"],
            "no_eligible": m["no_eligible"],
            "yes_at_risk": m["yes_at_risk"],
            "no_at_risk":  m["no_at_risk"],
            "pool":        m["pool"],
            "neg_risk":    m["neg_risk"],
        })

    risk = _compute_risk()

    return {
        "ts":              datetime.now(timezone.utc).isoformat(),
        "exec_mode":       EXEC_MODE,
        "bot_running":     state["bot_running"],
        "bot_pid":         state["bot_pid"],
        "feed_p50_ms":     state["feed_p50_ms"],
        "feed_p99_ms":     state["feed_p99_ms"],
        "reconnect_count": state["reconnect_count"],
        "msg_per_sec":     state["msg_per_sec"],
        "rest_ok":         state["rest_ok"],
        "tel_rows":        state["tel_rows"],
        "wallet": {
            "address": WALLET_ADDR,
            "pusd":    state["pusd"],
            "pol":     state["pol"],
            "ts":      state["wallet_ts"],
        },
        "markets":         markets_out,
        "risk":            risk,
    }

# ─── FastAPI app ──────────────────────────────────────────────────────────────
app = FastAPI(title="Polymarket LP Dashboard")

@app.on_event("startup")
async def startup():
    asyncio.create_task(_poll_prices())
    asyncio.create_task(_poll_telemetry())
    asyncio.create_task(_poll_bot_log())
    asyncio.create_task(_poll_wallet())
    asyncio.create_task(_broadcast_loop())

@app.get("/", response_class=HTMLResponse)
async def root():
    return HTML_FILE.read_text()

@app.get("/state")
async def get_state():
    return _build_payload()

@app.websocket("/ws")
async def ws_endpoint(ws: WebSocket):
    await ws.accept()
    _clients.add(ws)
    try:
        # Send current state immediately
        await ws.send_text(json.dumps(_build_payload()))
        while True:
            # Keep alive; broadcasts are driven by _broadcast_loop
            await asyncio.wait_for(ws.receive_text(), timeout=60)
    except (WebSocketDisconnect, asyncio.TimeoutError, Exception):
        pass
    finally:
        _clients.discard(ws)
