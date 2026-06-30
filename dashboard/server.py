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

import asyncio, json, math, os, re, sys, time, csv
from collections import defaultdict, deque
from datetime import datetime, timezone
from pathlib import Path
from typing import Dict, List, Optional, Set
import urllib.request
import urllib.error

from fastapi import FastAPI, WebSocket, WebSocketDisconnect, Request, HTTPException
from fastapi.responses import HTMLResponse
import secrets, subprocess

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
    # live resting orders (what the money is invested in)
    "open_orders":      [],
    "invested_usd":     0.0,
    "orders_ts":        None,
    # filled inventory — tokens we actually hold (NOT resting orders)
    "positions":        [],
    "positions_value":  0.0,
    "positions_pnl":    0.0,
    "positions_ts":     None,
    # reward-program metrics per market + actual earnings
    "rewards":          {},
    "earnings_today":   0.0,
    "rewards_ts":       None,
    # live market screener (top reward markets) + PnL/earnings time series
    "screener":         [],
    "screener_ts":      None,
    "pnl_hist":         deque(maxlen=500),
    # all-time earnings: per-day rewards + running cumulative (for the chart toggle)
    "earnings_history": [],
    "earnings_alltime": 0.0,
    "earnings_hist_ts": None,
    # portfolio value time series (cash + position value + position P&L) for the chart
    "port_hist":        deque(maxlen=500),
    # per-market reward breakdown for "today" (which market paid what)
    "earnings_by_market": [],
    # account activity ledger (trades / redeems / rewards) + consolidated P&L
    "activity":         [],
    "activity_ts":      None,
    "pnl_summary":      {},
}

# Persist the portfolio-value series to disk so a dashboard restart (e.g. after a
# config change adds a market) doesn't wipe the chart's history. Best-effort.
PORT_HIST_FILE = BASE_DIR / "logs" / "port_hist.json"
try:
    for _p in (json.load(open(PORT_HIST_FILE)) or [])[-500:]:
        state["port_hist"].append(_p)
except Exception:
    pass

def _save_port_hist():
    try:
        PORT_HIST_FILE.parent.mkdir(parents=True, exist_ok=True)
        tmp = PORT_HIST_FILE.with_suffix(".tmp")
        tmp.write_text(json.dumps(list(state["port_hist"])))
        tmp.replace(PORT_HIST_FILE)
    except Exception:
        pass

def _cond_name(cond: str):
    """Resolve a condition_id (rewards row) to a human market name when we know it."""
    if not cond:
        return None
    c = cond.lower()
    for m in MARKETS:
        if str(m.get("condition_id", "")).lower() == c:
            return m["name"]
    for r in _SCREENER_ALL:
        if str(r.get("condition_id", "")).lower() == c:
            return r.get("question")
    return None

_clients: Set[WebSocket] = set()

# ─── On-chain wallet snapshot (collateral + gas) ──────────────────────────────
# Funds live in the DEPOSIT WALLET (the maker); gas (POL) lives on the owner EOA.
EOA_ADDR       = "0x4E3b143938947039b2F0b13BD1038683DE57851F"   # owner/signer, holds gas
DEPOSIT_WALLET = "0x832317706479bb6762741B9b9ba568bb86fFfFF0"   # maker, holds pUSD collateral
WALLET_ADDR    = DEPOSIT_WALLET                                  # the address shown on the dashboard
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
        d = "0x70a08231" + "0" * 24 + DEPOSIT_WALLET[2:].lower()
        pusd = _rpc("eth_call", [{"to": PUSD_TOKEN, "data": d}, "latest"])   # collateral in the deposit wallet
        pol = _rpc("eth_getBalance", [EOA_ADDR, "latest"])                   # gas on the owner EOA
        state["pusd"] = int(pusd, 16) / 1e6 if pusd and pusd != "0x" else None
        state["pol"] = int(pol, 16) / 1e18 if pol else None
        state["wallet_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_wallet():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_wallet)
        await asyncio.sleep(30)

# ─── Live open orders (what the money is actually invested in) ─────────────────
# Polls the CLOB for resting orders on the deposit wallet via py-clob-client-v2.
sys.path.insert(0, str(BASE_DIR / ".pmlibs"))
_clob_client = None

def _creds_env(path="/home/ubuntu/.pm_creds.env"):
    o = {}
    try:
        for ln in open(path):
            ln = ln.strip()
            if ln.startswith("export "):
                ln = ln[7:]
            if "=" in ln and not ln.startswith("#"):
                k, v = ln.split("=", 1)
                o[k.strip()] = v.strip().strip('"').strip("'")
    except Exception:
        pass
    return o

def _token_label(asset_id: str):
    for m in MARKETS:
        if asset_id == m.get("yes_token"): return m["name"][:28], "YES"
        if asset_id == m.get("no_token"):  return m["name"][:28], "NO"
    return "…" + str(asset_id)[-6:], "?"

def _get_clob():
    global _clob_client
    if _clob_client is None:
        from py_clob_client_v2 import ClobClient, ApiCreds
        e = _creds_env(); key = open("/home/ubuntu/.pm_signer_key").read().strip()
        _clob_client = ClobClient(host=CLOB_BASE, chain_id=137, key=key,
                                  creds=ApiCreds(e["PM_API_KEY"], e["PM_API_SECRET"], e["PM_API_PASSPHRASE"]),
                                  signature_type=3, funder=DEPOSIT_WALLET)
    return _clob_client

def _read_orders():
    try:
        oo = _get_clob().get_open_orders() or []
        rows, invested = [], 0.0
        for o in oo:
            aid = o.get("asset_id", "")
            name, outcome = _token_label(aid)
            full = name
            for m in MARKETS:
                if aid == m.get("yes_token") or aid == m.get("no_token"):
                    full = m["name"]; break
            price = float(o.get("price", 0)); size = float(o.get("original_size", 0))
            invested += price * size
            rows.append({"market": name, "market_full": full, "outcome": outcome,
                         "side": o.get("side"), "price": price, "size": size,
                         "asset": aid, "cond": o.get("market"),
                         "matched": float(o.get("size_matched", 0) or 0)})
        state["open_orders"] = rows
        state["invested_usd"] = round(invested, 2)
        state["orders_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_orders():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_orders)
        await asyncio.sleep(10)

# ─── Filled inventory (tokens we actually hold) ────────────────────────────────
# Resting orders ≠ holdings. When a maker order fills we acquire YES/NO tokens that
# sit in the deposit wallet until sold or resolved. The CLOB "open orders" call does
# NOT show these, so a filled position was invisible on the dashboard. This polls
# Polymarket's data-api for the deposit wallet's actual positions.
def _read_positions():
    try:
        url = f"https://data-api.polymarket.com/positions?user={DEPOSIT_WALLET}"
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=12) as r:
            raw = json.loads(r.read())
        rows, val, pnl = [], 0.0, 0.0
        for p in (raw or []):
            sz = float(p.get("size", 0) or 0)
            if abs(sz) < 1e-9:
                continue
            cv = float(p.get("currentValue", 0) or 0)
            cp = float(p.get("cashPnl", 0) or 0)
            val += cv; pnl += cp
            rows.append({
                "title":      p.get("title") or p.get("slug") or "?",
                "outcome":    p.get("outcome"),
                "size":       round(sz, 4),
                "avg_price":  p.get("avgPrice"),
                "cur_price":  p.get("curPrice"),
                "value":      round(cv, 2),
                "pnl":        round(cp, 2),
                "pct_pnl":    round(float(p.get("percentPnl", 0) or 0), 1),
                "redeemable": bool(p.get("redeemable")),
                "end_date":   p.get("endDate"),
                "cond":       p.get("conditionId"),
                "asset":      p.get("asset"),
            })
        state["positions"] = rows
        state["positions_value"] = round(val, 2)
        state["positions_pnl"] = round(pnl, 2)
        state["positions_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_positions():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_positions)
        # sample portfolio value (cash + position value + position P&L) for the chart.
        # skip until the wallet poller has a real cash figure, so we never plot a fake $0 dip.
        try:
            import time as _t
            cash = state.get("pusd")
            if cash is not None:
                pv = state.get("positions_value", 0.0)
                state["port_hist"].append({"t": int(_t.time()), "total": round(cash + pv, 2),
                                           "cash": round(cash, 2), "pos": round(pv, 2),
                                           "pnl": state.get("positions_pnl", 0.0)})
                _save_port_hist()
        except Exception:
            pass
        await asyncio.sleep(15)

# ─── Account history (activity ledger) + consolidated P&L ──────────────────────
# data-api /activity is the on-chain ledger: TRADE / REDEEM / REWARD / CONVERSION /
# SPLIT / MERGE rows with usdc amounts. We surface it as a "history" feed and roll a
# consolidated P&L: rewards (reliable) + realized trading + unrealized (mark, caveat).
def _read_activity():
    try:
        url = f"https://data-api.polymarket.com/activity?user={DEPOSIT_WALLET}&limit=200"
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=12) as r:
            raw = json.loads(r.read())
        acts = []
        for a in (raw or []):
            acts.append({
                "ts":      a.get("timestamp"),
                "type":    a.get("type"),            # TRADE / REDEEM / REWARD / …
                "title":   a.get("title") or "?",
                "outcome": a.get("outcome"),
                "side":    a.get("side"),            # BUY / SELL (trades only)
                "size":    a.get("size"),
                "usd":     round(float(a.get("usdcSize", 0) or 0), 2),
                "price":   a.get("price"),
                "cond":    a.get("conditionId"),
                "tx":      a.get("transactionHash"),
            })
        state["activity"] = acts
        state["activity_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass
    # consolidated P&L summary (best-effort, labelled honestly on the frontend)
    try:
        pos = state.get("positions", [])
        unreal = round(sum(float(p.get("pnl", 0) or 0) for p in pos), 2)   # current mark P&L
        rewards = float(state.get("earnings_alltime", 0) or 0)
        state["pnl_summary"] = {
            "rewards":    round(rewards, 4),
            "unrealized": unreal,
            "cash":       round(float(state.get("pusd") or 0), 2),
            "pos_value":  round(float(state.get("positions_value") or 0), 2),
            "total_value": round(float(state.get("pusd") or 0) + float(state.get("positions_value") or 0), 2),
        }
    except Exception:
        pass

async def _poll_activity():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_activity)
        await asyncio.sleep(45)

# ─── Reward-program metrics (the Polymarket "Rewards" page, per market) ────────
# max_spread, min_size, $/day rate, competition (market_competitiveness),
# current price, your actual earnings today, and a buffed/nerfed flag derived by
# tracking each market's rate_per_day across polls.
_rate_hist: Dict[str, float] = {}

def _read_rewards():
    try:
        c = _get_clob()
        import datetime
        day = datetime.date.today().isoformat()
        earn = {}
        try:
            for e in (c.get_earnings_for_user_for_day(day) or []):
                earn[e.get("condition_id")] = earn.get(e.get("condition_id"), 0.0) + float(e.get("earnings", 0) or 0)
        except Exception:
            pass
        rw, total = {}, 0.0
        for m in MARKETS:
            cid = m["condition_id"]
            try:
                raw = c.get_raw_rewards_for_market(cid) or []
            except Exception:
                raw = []
            if not raw:
                continue
            r = raw[0]
            cfg = (r.get("rewards_config") or [{}])[0]
            rate = cfg.get("rate_per_day")
            prev = _rate_hist.get(cid)
            bn = "—"
            if prev is not None and rate is not None:
                bn = "BUFFED" if rate > prev else "NERFED" if rate < prev else "flat"
            if rate is not None:
                _rate_hist[cid] = rate
            toks = {t.get("outcome", "").lower(): t.get("price") for t in r.get("tokens", [])}
            e = float(earn.get(cid, 0.0)); total += e
            rw[cid] = {
                "max_spread":      r.get("rewards_max_spread"),
                "min_size":        r.get("rewards_min_size"),
                "rate_day":        rate,
                "competitiveness": r.get("market_competitiveness"),
                "yes_price":       toks.get("yes"),
                "no_price":        toks.get("no"),
                "earnings":        round(e, 4),
                "buff_nerf":       bn,
            }
        state["rewards"] = rw
        state["earnings_today"] = round(total, 4)
        state["rewards_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_rewards():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_rewards)
        # sample the reward-earnings time series for the PnL chart
        try:
            import time as _t
            state["pnl_hist"].append({"t": int(_t.time()), "earn": state.get("earnings_today", 0.0)})
        except Exception:
            pass
        await asyncio.sleep(30)

# ─── All-time earnings history (per-day rewards, cumulative) ───────────────────
# get_earnings_for_user_for_day is per-UTC-day; "today" resets at 00:00. To show an
# all-time view we walk each day from account start to today and accumulate.
import datetime as _dtmod
EARN_START = _dtmod.date(2026, 6, 25)   # a couple days before first funding; days<start earn $0

def _read_earnings_history():
    try:
        c = _get_clob()
        today = _dtmod.date.today()
        start = max(EARN_START, today - _dtmod.timedelta(days=90))
        hist, cum = [], 0.0
        by_market = {}
        d = start
        while d <= today:
            iso = d.isoformat()
            try:
                rows = c.get_earnings_for_user_for_day(iso) or []
                day_earn = sum(float(x.get("earnings", 0) or 0) for x in rows)
            except Exception:
                rows, day_earn = [], 0.0
            if d == today:
                # which market(s) paid today — resolve condition_id -> name
                for x in rows:
                    cond = x.get("condition_id") or x.get("market") or ""
                    earn = float(x.get("earnings", 0) or 0)
                    if cond not in by_market:
                        by_market[cond] = 0.0
                    by_market[cond] += earn
            cum += day_earn
            hist.append({"date": iso, "day": round(day_earn, 4), "cum": round(cum, 4)})
            d += _dtmod.timedelta(days=1)
        breakdown = [{"cond": k, "name": _cond_name(k) or ("…" + str(k)[-6:]),
                      "earn": round(v, 4)} for k, v in by_market.items()]
        breakdown.sort(key=lambda r: -r["earn"])
        state["earnings_by_market"] = breakdown
        state["earnings_history"] = hist
        state["earnings_alltime"] = round(cum, 4)
        state["earnings_hist_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

async def _poll_earnings_history():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_earnings_history)
        await asyncio.sleep(300)   # 5 min — earnings finalize slowly

# ─── Market screener: top reward markets pulled live from the CLOB ─────────────
def _dte(iso):
    if not iso:
        return None
    try:
        import datetime
        end = datetime.datetime.fromisoformat(iso.replace("Z", "+00:00"))
        return (end - datetime.datetime.now(datetime.timezone.utc)).days
    except Exception:
        return None

_SCREENER_ALL = []   # full processed reward-market list, for /search

def _read_screener():
    try:
        c = _get_clob()
        items, cur = [], None
        for _ in range(12):   # ~8 pages × 1000 covers the full reward-market universe
            page = c.get_sampling_markets(next_cursor=cur) if cur else c.get_sampling_markets()
            data = page.get("data", page) if isinstance(page, dict) else page
            items.extend(data)
            cur = page.get("next_cursor") if isinstance(page, dict) else None
            if not cur or cur == "LTE=":
                break
        rows = []
        for m in items:
            if not (m.get("active") and m.get("accepting_orders") and m.get("enable_order_book")):
                continue
            rw = m.get("rewards") or {}
            rate = sum(float(r.get("rewards_daily_rate", 0) or 0) for r in (rw.get("rates") or []))
            if rate <= 0:
                continue
            toks = {t.get("outcome", "").lower(): t for t in m.get("tokens", [])}
            yes, no = toks.get("yes", {}), toks.get("no", {})
            rows.append({
                "question":    m.get("question"),
                "condition_id": m.get("condition_id"),
                "slug":        m.get("market_slug"),
                "rate_day":    round(rate, 2),
                "max_spread":  rw.get("max_spread"),
                "min_size":    rw.get("min_size"),
                "yes_price":   yes.get("price"),
                "no_price":    no.get("price"),
                "yes_token":   yes.get("token_id"),
                "no_token":    no.get("token_id"),
                "neg_risk":    m.get("neg_risk"),
                "tick":        m.get("minimum_tick_size"),
                "dte":         _dte(m.get("end_date_iso")),
                "end_date":    m.get("end_date_iso"),
                "description": (m.get("description") or "")[:700],
                "tags":        m.get("tags") or [],
            })
        rows.sort(key=lambda r: r["rate_day"], reverse=True)
        # ── exitability: probe top-by-rate books for spread + depth, then re-rank ──
        # The Micron lesson: rank by *tradeable* reward, not raw reward. A fat rate in a
        # 36¢-wide/thin book is a trap. score = rate × spread_factor × depth_factor.
        _annotate_exitability(c, rows[:150])
        for r in rows:
            r.setdefault("exit_score", round(r["rate_day"] * 0.02, 3))  # unprobed → sink
            r.setdefault("exit_label", "?"); r.setdefault("spread_c", None)
            r.setdefault("depth", None); r.setdefault("best_bid", None); r.setdefault("best_ask", None)
            # two-sided affordability: capital to rest min_size on BOTH legs at once.
            # cost ≈ min_size × (yes_price + no_price)  (≈ min_size since yes+no≈$1).
            # This is THE filter for small capital: can we even qualify for rewards here?
            try:
                msz = float(r.get("min_size") or 0)
                yp, np_ = r.get("yes_price"), r.get("no_price")
                r["two_sided_cost"] = (round(msz * (float(yp) + float(np_)), 2)
                                       if msz and yp is not None and np_ is not None else None)
            except Exception:
                r["two_sided_cost"] = None
        rows.sort(key=lambda r: r["exit_score"], reverse=True)
        global _SCREENER_ALL
        _SCREENER_ALL = rows
        state["screener"] = rows[:60]
        state["screener_ts"] = datetime.now(timezone.utc).isoformat()
    except Exception:
        pass

def _annotate_exitability(c, subset):
    """Fetch YES-token books in batches; annotate each row with spread/depth/exit score."""
    from py_clob_client_v2.clob_types import BookParams
    toks = [r["yes_token"] for r in subset if r.get("yes_token")]
    books = {}
    for i in range(0, len(toks), 50):
        chunk = toks[i:i+50]
        try:
            for b in (c.get_order_books([BookParams(token_id=t) for t in chunk]) or []):
                books[b.get("asset_id")] = b
        except Exception:
            pass
    for r in subset:
        b = books.get(r.get("yes_token"))
        bids = (b or {}).get("bids") or []
        asks = (b or {}).get("asks") or []
        if not bids or not asks:
            r["exit_score"] = round(r["rate_day"] * 0.02, 3)
            r["exit_label"] = "?"; r["spread_c"] = None; r["depth"] = None
            r["best_bid"] = None; r["best_ask"] = None
            continue
        bb = max(bids, key=lambda x: float(x["price"]))
        ba = min(asks, key=lambda x: float(x["price"]))
        best_bid, best_ask = float(bb["price"]), float(ba["price"])
        spread_c = round((best_ask - best_bid) * 100, 2)
        depth = round(min(float(bb["size"]), float(ba["size"])), 1)
        band = float(r.get("max_spread") or 2.0)            # reward band, in cents
        msz  = float(r.get("min_size") or 20)
        spread_factor = 1.0 if spread_c <= band else max(0.0, band / spread_c)
        depth_factor  = min(depth / msz, 1.0) if msz else 0.0
        r["best_bid"] = best_bid; r["best_ask"] = best_ask
        r["spread_c"] = spread_c; r["depth"] = depth
        r["exit_score"] = round(r["rate_day"] * spread_factor * depth_factor, 3)
        r["exit_label"] = ("GOOD" if spread_factor >= 0.5 and depth_factor >= 1.0
                           else "OK" if spread_factor >= 0.25 and depth_factor >= 0.5
                           else "POOR")

async def _poll_screener():
    while True:
        await asyncio.get_event_loop().run_in_executor(None, _read_screener)
        await asyncio.sleep(60)

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
            "condition_id":m["condition_id"],
            "name":        m["name"],
            "yes_token":   m["yes_token"],
            "no_token":    m["no_token"],
            "idx":         MARKETS.index(m),
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
        "open_orders":     state["open_orders"],
        "invested_usd":    state["invested_usd"],
        "positions":       state["positions"],
        "positions_value": state["positions_value"],
        "positions_pnl":   state["positions_pnl"],
        "portfolio_total": round((state["pusd"] or 0.0) + state["positions_value"], 2),
        "rewards":         state["rewards"],
        "earnings_today":  state["earnings_today"],
        "screener":        state["screener"],
        "pnl_hist":        list(state["pnl_hist"]),
        "earnings_history":state["earnings_history"],
        "earnings_alltime":state["earnings_alltime"],
        "earnings_by_market": state["earnings_by_market"],
        "activity":        state["activity"],
        "pnl_summary":     state["pnl_summary"],
        "port_hist":       list(state["port_hist"]),
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
    asyncio.create_task(_poll_orders())
    asyncio.create_task(_poll_positions())
    asyncio.create_task(_poll_activity())
    asyncio.create_task(_poll_rewards())
    asyncio.create_task(_poll_earnings_history())
    asyncio.create_task(_poll_screener())
    asyncio.create_task(_broadcast_loop())

@app.get("/", response_class=HTMLResponse)
async def root():
    return HTML_FILE.read_text()

@app.get("/state")
async def get_state():
    return _build_payload()

# ─── Control panel (token-protected order entry + MM start/stop) ───────────────
# All execute actions require header  X-Control-Token: <token>.  Token comes from
# $DASH_CONTROL_TOKEN, else generated + written to dashboard/.control_token (0600)
# and printed in the log. Orders are POST-ONLY and capped — a fat-finger guard.
CONTROL_TOKEN = os.getenv("DASH_CONTROL_TOKEN") or secrets.token_hex(12)
CTRL_MAX_NOTIONAL = float(os.getenv("DASH_MAX_ORDER_NOTIONAL", "19"))
try:
    _tp = Path(__file__).parent / ".control_token"
    _tp.write_text(CONTROL_TOKEN); os.chmod(_tp, 0o600)
except Exception:
    pass
print(f"[dashboard] control token: {CONTROL_TOKEN}  (set $DASH_CONTROL_TOKEN to fix it)", flush=True)

def _auth(req: Request):
    if req.headers.get("X-Control-Token") != CONTROL_TOKEN:
        raise HTTPException(status_code=401, detail="invalid control token")

@app.post("/control/place")
async def control_place(req: Request):
    _auth(req)
    b = await req.json()
    token_id = str(b["token_id"]); side = str(b["side"]).upper()
    price = float(b["price"]); size = float(b["size"])
    if side not in ("BUY", "SELL"):
        raise HTTPException(400, "side must be BUY or SELL")
    if not (0.01 <= price <= 0.99):
        raise HTTPException(400, "price out of range")
    if price * size > CTRL_MAX_NOTIONAL:
        raise HTTPException(400, f"notional {price*size:.2f} > cap {CTRL_MAX_NOTIONAL}")
    def _do():
        from py_clob_client_v2 import OrderArgsV2, OrderType, PartialCreateOrderOptions
        c = _get_clob()
        neg = c.get_neg_risk(token_id); tick = c.get_tick_size(token_id)
        return c.create_and_post_order(
            OrderArgsV2(token_id=token_id, price=price, size=size, side=side),
            options=PartialCreateOrderOptions(tick_size=tick, neg_risk=neg),
            order_type=OrderType.GTC, post_only=True)
    r = await asyncio.get_event_loop().run_in_executor(None, _do)
    ok = isinstance(r, dict) and r.get("success")
    asyncio.get_event_loop().run_in_executor(None, _read_orders)
    return {"ok": bool(ok), "raw": r}

@app.post("/control/close_position")
async def control_close_position(req: Request):
    # Sell held inventory at a chosen price. NOT post-only and NOT notional-capped:
    # you're selling tokens you already own (can't overspend cash), and you may want
    # to price into the bid to fill immediately. Exchange rejects selling > you hold.
    _auth(req)
    b = await req.json()
    token_id = str(b["token_id"]); price = float(b["price"]); size = float(b["size"])
    if not (0.01 <= price <= 0.99):
        raise HTTPException(400, "price out of range")
    if size <= 0:
        raise HTTPException(400, "size must be > 0")
    def _do():
        from py_clob_client_v2 import OrderArgsV2, OrderType, PartialCreateOrderOptions
        c = _get_clob()
        neg = c.get_neg_risk(token_id); tick = c.get_tick_size(token_id)
        return c.create_and_post_order(
            OrderArgsV2(token_id=token_id, price=price, size=size, side="SELL"),
            options=PartialCreateOrderOptions(tick_size=tick, neg_risk=neg),
            order_type=OrderType.GTC, post_only=False)
    r = await asyncio.get_event_loop().run_in_executor(None, _do)
    ok = isinstance(r, dict) and r.get("success")
    asyncio.get_event_loop().run_in_executor(None, _read_orders)
    asyncio.get_event_loop().run_in_executor(None, _read_positions)
    return {"ok": bool(ok), "raw": r}

@app.get("/market_rewards")
async def market_rewards(cid: str):
    # Lazy per-market reward detail (competition level + config) for the screener
    # detail box. Read-only, no auth. One CLOB call per expanded row.
    def _do():
        try:
            raw = _get_clob().get_raw_rewards_for_market(cid) or []
        except Exception:
            return {}
        if not raw:
            return {}
        r = raw[0]
        toks = {t.get("outcome", "").lower(): t.get("price") for t in r.get("tokens", [])}
        cfg = (r.get("rewards_config") or [{}])[0]
        return {
            "min_size":        r.get("rewards_min_size"),
            "max_spread":      r.get("rewards_max_spread"),
            "competitiveness": r.get("market_competitiveness"),
            "rate_day":        cfg.get("rate_per_day"),
            "yes_price":       toks.get("yes"),
            "no_price":        toks.get("no"),
        }
    return await asyncio.get_event_loop().run_in_executor(None, _do)

@app.post("/control/cancel_all")
async def control_cancel_all(req: Request):
    _auth(req)
    r = await asyncio.get_event_loop().run_in_executor(None, lambda: _get_clob().cancel_all())
    asyncio.get_event_loop().run_in_executor(None, _read_orders)
    return {"ok": True, "raw": r}

@app.post("/control/abort")
async def control_abort(req: Request):
    """PANIC: stop every MM process AND cancel every resting order."""
    _auth(req)
    subprocess.run(["pkill", "-f", "mm_gateway.py"])
    r = await asyncio.get_event_loop().run_in_executor(None, lambda: _get_clob().cancel_all())
    asyncio.get_event_loop().run_in_executor(None, _read_orders)
    return {"ok": True, "stopped_mm": True, "cancel_all": r}

@app.get("/search")
async def search(q: str = ""):
    """Search the live reward-market universe by question text (read-only, no auth)."""
    q = (q or "").strip().lower()
    if not q:
        return []
    res = [r for r in _SCREENER_ALL if q in (r.get("question") or "").lower()]
    res.sort(key=lambda r: r["rate_day"], reverse=True)
    return res[:80]

@app.get("/watchrows")
async def watchrows(conds: str = ""):
    """Return full screener rows for a comma-separated list of condition_ids (read-only).
    Powers the watchlist: the browser stores starred condition_ids, this resolves their
    live data from the processed reward-market universe (so stars outside the top-60 work)."""
    want = [c.strip().lower() for c in (conds or "").split(",") if c.strip()]
    if not want:
        return []
    by_cond = {str(r.get("condition_id", "")).lower(): r for r in _SCREENER_ALL}
    return [by_cond[c] for c in want if c in by_cond]

@app.post("/control/mm_start")
async def control_mm_start(req: Request):
    _auth(req)
    b = await req.json()
    contract = int(b.get("contract", 0)); size = float(b.get("size", 20)); hs = float(b.get("half_spread", 0.02))
    base = Path(__file__).parent.parent
    subprocess.run(["pkill", "-f", "mm_gateway.py"])  # stop any existing instance
    await asyncio.sleep(1.2)
    env = dict(os.environ); env["PYTHONPATH"] = str(base / ".pmlibs")
    logf = open(base / "logs" / "mm.log", "a")
    p = subprocess.Popen(
        ["python3", str(base / "tools" / "mm_gateway.py"),
         "--contracts", str(contract), "--size", str(size), "--half-spread", str(hs),
         "--max-order-notional", "19", "--max-total-notional", "22", "--interval", "8", "--live"],
        cwd=str(base), env=env, stdout=logf, stderr=logf,
        stdin=subprocess.DEVNULL, start_new_session=True)  # detached: survives dashboard restarts
    return {"ok": True, "pid": p.pid, "contract": contract, "size": size, "half_spread": hs}

@app.post("/control/mm_stop")
async def control_mm_stop(req: Request):
    _auth(req)
    subprocess.run(["pkill", "-f", "mm_gateway.py"])
    return {"ok": True, "stopped": True}

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
