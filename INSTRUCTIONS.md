# Polymarket bot — operator runbook (LIVE)

Everything you need to run, monitor, and understand the live trading system.
Last updated 2026-06-28.

> **TL;DR:** The bot trades live on Polymarket through a **deposit wallet** using
> the **Python connector** (`py-clob-client-v2`, sigType-3 / POLY_1271). The fast
> C++ engine can drive that connector via a relay. Funds live in the deposit wallet
> `0x8323…`; the EOA `0x4E3b…` is the signer/owner. To **earn now**, run
> `tools/mm_gateway.py` (proven live). Everything is **post-only, cash-only,
> capped, and cancels-all on exit.**

---

## 1. How it works (the path we proved)

Polymarket V2 does **not** let a bare wallet trade — new accounts get a
**deposit wallet** (an ERC-7739 smart wallet) that must sign orders with
**signatureType 3 (POLY_1271 / ERC-7739 TypedDataSign)**. Our fast C++ signer only
does plain-ECDSA (sigType 0/1/2), so the **Python SDK does the sigType-3 signing**.

```
            ┌──────────────── on-chain (Polygon) ────────────────┐
EOA 0x4E3b… (signer/owner, holds the key)  ──owns──▶  Deposit wallet 0x8323…
   key: /home/ubuntu/.pm_signer_key                   holds pUSD + has allowances
                                                       = the "maker" Polymarket wants
            └─────────────────────────────────────────────────────┘

Order flow (two ways to drive it):

  (A) Python market-maker  ──────────────────────────────────────────┐
      tools/mm_gateway.py                                             │
                                                                      ▼
  (B) C++ HFT engine ──relay──▶ Python connector ──sigType-3 sign──▶ Polymarket CLOB
      build/arb_detector        tools/order_gateway_server.py
      (exec_mode=relay)         (py-clob-client-v2)
```

- **Auth:** the L2 API key in `/home/ubuntu/.pm_creds.env` is bound to the **EOA**
  and authenticates every request (it does *not* need to be bound to the deposit
  wallet — that was the key insight that unblocked everything).
- **Cash-only, two-sided:** we only ever place **BUY** orders — `BUY YES` *and*
  `BUY NO`. A bid on NO at price `p` is economically an ask on YES at `1−p`, so the
  pair quotes both sides and earns rewards **without holding any token inventory**
  (which a real SELL would require). Both legs are funded by pUSD.
- **post-only:** every order is post-only, so we never cross the book / never pay
  taker fees. Orders rest near mid and earn liquidity rewards.

---

## 2. Key facts

| thing | value |
|---|---|
| Signer EOA (owner) | `0x4E3b143938947039b2F0b13BD1038683DE57851F` |
| **Deposit wallet (maker, holds funds)** | `0x832317706479bb6762741B9b9ba568bb86fFfFF0` |
| Private key file | `/home/ubuntu/.pm_signer_key` (chmod 600 — never share/commit) |
| L2 API creds | `/home/ubuntu/.pm_creds.env` (chmod 600 — PM_API_* + PM_SIGNER_KEY) |
| Collateral | pUSD `0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB` (6 dp) |
| CLOB host | `https://clob.polymarket.com` (order version 2) |
| Polygon RPC (reads) | `https://polygon-bor-rpc.publicnode.com` (needs a browser User-Agent) |
| Python SDK | `py-clob-client-v2`, installed in `./.pmlibs` (gitignored) — use `PYTHONPATH=./.pmlibs` |

---

## 3. Everyday commands

### Check balances (deposit wallet is where the money is)
```bash
cd /home/ubuntu/polymarket
python3 - <<'PY'
import json, urllib.request
RPC="https://polygon-bor-rpc.publicnode.com"
PUSD="0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB"
DEP="0x832317706479bb6762741B9b9ba568bb86fFfFF0"
EOA="0x4E3b143938947039b2F0b13BD1038683DE57851F"
def rpc(m,p):
    r=urllib.request.Request(RPC,data=json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(),
        method="POST",headers={"Content-Type":"application/json","User-Agent":"Mozilla/5.0"})
    return json.loads(urllib.request.urlopen(r,timeout=12).read())["result"]
def bal(a): return int(rpc("eth_call",[{"to":PUSD,"data":"0x70a08231"+"0"*24+a[2:].lower()},"latest"]),16)/1e6
print("deposit wallet pUSD:", bal(DEP))
print("EOA            pUSD:", bal(EOA))
print("EOA            POL :", int(rpc("eth_getBalance",[EOA,"latest"]),16)/1e18)
PY
```

### Check open orders / positions on the CLOB
```bash
PYTHONPATH=./.pmlibs python3 - <<'PY'
from py_clob_client_v2 import ClobClient, ApiCreds
def env(p):
    o={}
    for ln in open(p):
        ln=ln.strip().removeprefix("export ")
        if "=" in ln and not ln.startswith("#"):
            k,v=ln.split("=",1); o[k.strip()]=v.strip().strip('"').strip("'")
    return o
e=env("/home/ubuntu/.pm_creds.env"); key=open("/home/ubuntu/.pm_signer_key").read().strip()
c=ClobClient(host="https://clob.polymarket.com", chain_id=137, key=key,
             creds=ApiCreds(e["PM_API_KEY"],e["PM_API_SECRET"],e["PM_API_PASSPHRASE"]),
             signature_type=3, funder="0x832317706479bb6762741B9b9ba568bb86fFfFF0")
print("open orders:", c.get_open_orders())
PY
```

### EMERGENCY: cancel everything
```bash
PYTHONPATH=./.pmlibs python3 - <<'PY'
from py_clob_client_v2 import ClobClient, ApiCreds
def env(p):
    o={}
    for ln in open(p):
        ln=ln.strip().removeprefix("export ")
        if "=" in ln and not ln.startswith("#"):
            k,v=ln.split("=",1); o[k.strip()]=v.strip().strip('"').strip("'")
    return o
e=env("/home/ubuntu/.pm_creds.env"); key=open("/home/ubuntu/.pm_signer_key").read().strip()
c=ClobClient(host="https://clob.polymarket.com", chain_id=137, key=key,
             creds=ApiCreds(e["PM_API_KEY"],e["PM_API_SECRET"],e["PM_API_PASSPHRASE"]),
             signature_type=3, funder="0x832317706479bb6762741B9b9ba568bb86fFfFF0")
print(c.cancel_all())
PY
```

---

## 4. Running mode (A) — the Python market-maker (recommended to earn now)

`tools/mm_gateway.py` is a self-contained, conservative MM. It quotes BUY-YES +
BUY-NO near mid on the markets in `config.live.json`, holds when mid is stable,
and flattens on exit.

```bash
cd /home/ubuntu/polymarket

# DRY-RUN (default): builds quotes, posts NOTHING. Always safe.
PYTHONPATH=./.pmlibs python3 tools/mm_gateway.py --contracts 0 --size 5 --duration 60

# LIVE: actually place orders (small + short first). Auto-flattens after --duration.
PYTHONPATH=./.pmlibs python3 tools/mm_gateway.py --contracts 0 --size 5 \
    --half-spread 0.02 --interval 8 --duration 120 --live

# run continuously (Ctrl-C cancels all and exits cleanly)
PYTHONPATH=./.pmlibs python3 tools/mm_gateway.py --contracts 0,1,2 --size 50 --live
```

**Flags:**
| flag | meaning | default |
|---|---|---|
| `--contracts` | comma indices into `config.live.json` `contracts[]` | `0` |
| `--size` | shares per order (≥ market `rewards_min_size` to earn rewards, often 50) | `5` |
| `--half-spread` | distance from mid, in price units (≤ market `rewards_max_spread` to earn) | `0.02` |
| `--max-order-notional` | $ cap per single order | `5` |
| `--max-total-notional` | $ cap across all live orders | `20` |
| `--interval` | loop period (seconds) | `5` |
| `--duration` | auto stop + flatten after N s (`0` = run forever) | `0` |
| `--live` | **actually place orders** (omit = dry-run) | off |

> To actually **earn rewards** (not just test mechanics), use `--size` ≥ the
> market's `rewards_min_size` (commonly 50 shares ≈ $6/order) and `--half-spread`
> ≤ its `rewards_max_spread`. Below min-size orders rest fine but don't score.

---

## 5. Running mode (B) — C++ HFT engine → Python connector (relay)

This drives the **fast C++ strategy** but executes through the Python connector.

**Step 1 — start the connector** (keep it running; it's the thing that signs + POSTs):
```bash
cd /home/ubuntu/polymarket
export ORDER_GW_TOKEN="$(openssl rand -hex 16)"   # shared secret; export the SAME value for the bot
PYTHONPATH=./.pmlibs python3 tools/order_gateway_server.py --port 8765          # LIVE
# add --dry to build requests but NOT post (for wiring tests)
```
It prints `signer`/`funder` and (if you didn't set `ORDER_GW_TOKEN`) a generated
token. It **cancels-all on Ctrl-C / SIGTERM**.

**Step 2 — run the bot in relay mode**, in the same shell (so `ORDER_GW_TOKEN` is set):
```bash
# config must set "exec_mode":"relay" and "shadow_executor_enabled":true.
# live_arm=false (default) => build intents but DON'T forward (dry). Flip to true to send.
./build/arb_detector --config config.live.json
```
The bot's OMS forwards create/cancel to `127.0.0.1:8765`; the connector signs
sigType-3 and POSTs. Relay config keys: `relay_host` (default `127.0.0.1`),
`relay_port` (default `8765`); the bearer token comes from `$ORDER_GW_TOKEN` only.

> ⚠️ **Open caveat before a fully-armed C++ run:** the C++ reward quoter rests a
> **bid *and* an ask on the same token**. An ask (SELL) needs token inventory the
> cash-only deposit wallet doesn't hold, so those legs would be rejected. Until the
> C++ quoter is adapted to the cash-only **BUY-YES + BUY-NO** structure (like
> `mm_gateway.py`), prefer mode (A) for real earning. The relay plumbing itself is
> built, wired, and tested.

---

## 6. Connector HTTP API (mode B internals / manual control)

Localhost only, bearer-auth (`Authorization: Bearer $ORDER_GW_TOKEN`):

| method | path | body | does |
|---|---|---|---|
| GET | `/health` | — | `{ok,signer,funder,dry,open}` (no auth) |
| GET | `/open_orders` | — | list of resting orders |
| POST | `/place` | `{token_id,side,price,size,neg_risk?,tick_size?,post_only?}` | sign + post one order |
| POST | `/cancel` | `{order_id}` | cancel one order |
| POST | `/cancel_all` | `{}` | cancel everything |

Caps: `GW_MAX_ORDER_NOTIONAL` (default $10), `GW_MAX_ORDER_SIZE` (200 shares).

---

## 7. Dashboard (monitor + control everything)

```bash
cd /home/ubuntu/polymarket/dashboard && ./start.sh    # http://<EC2-ip>:8080
```
> Open **TCP 8080 → Source: My IP** in the EC2 security group (the dashboard has no
> login; restrict it to your IP). Instance `i-0b579563952ae62b1`, SG `launch-wizard-2`.

**Panels:**
- **KPI strip** — *Invested $* (actual resting-order notional, **not** a hypothetical),
  *Earned Today* (real rewards — **click it for a PnL/earnings chart**), *Collateral pUSD*
  (deposit wallet), *Gas POL*, Net Delta, Max Loss, VaR.
- **MARKETS** — your traded markets: live YES/NO, bid/ask, spread, pool.
- **OPEN ORDERS** — exactly what your money is in right now (market, side, price, size, $).
- **REWARDS** — the Polymarket rewards-page metrics per traded market: price Y/N,
  **max spread**, **min size**, **$/day rate**, **DTE**, **competition** (LOW/MED/HIGH +
  raw `market_competitiveness`), **earned today**, and **RATE Δ** (▲BUFFED / ▼NERFED,
  derived by tracking the daily rate across polls).
- **CONTROLS** — token-protected order entry (see below).
- **SCREENER · TOP REWARD MARKETS** — the top 60 reward markets pulled live from the
  CLOB, sorted by $/day. Columns: market (full name), YES/NO price, $/day, spread, min,
  **DTE**, tick. **Click any row** to expand details: condition_id, end date + DTE,
  resolution **description/rules**, and a **quick-quote** form (BUY YES/NO at your price/size).
  **Search bar** — type any market name (Micron, Bitcoin, France…) to search the *full*
  reward-market universe (~7,300 markets, paginated from the API); Clear returns to the top 60.
- **PORTFOLIO RISK / SYSTEM** — stress scenarios + feed/process health.

### Control panel (place/cancel orders + start/stop the MM from the browser)
All execute actions require the **control token**:
```bash
cat /home/ubuntu/polymarket/dashboard/.control_token     # the token (stable across restarts)
```
Paste it into the CONTROLS box → **Unlock** (stored in your browser). Then you can:
- **Place order** — pick market → YES/NO → BUY/SELL → price → shares. *Always post-only,
  capped ≤ $19/order* (a fat-finger guard).
- **Cancel ALL** — flatten every resting order.
- **Start MM / Stop MM** — launch or stop the auto market-maker on a chosen market/size
  (started detached, so it survives a dashboard restart).
- **⛔ ABORT** — the big red button: **one click stops every MM process AND cancels every
  resting order** (panic flatten → all cash). Use it if anything looks wrong.
- **Quick-quote from the screener** — expand any screener row → place a BUY on that exact
  market at your price.

The token is generated once and saved to `dashboard/.control_token`; `cat` just *reads*
it (doesn't make a new one). It only changes if you delete that file. Pin your own with
`export DASH_CONTROL_TOKEN=...` before `start.sh`.

> ⚠️ Anyone who can reach :8080 **and** has the token can place/cancel orders and
> start/stop the MM (it **cannot** withdraw funds — orders are post-only + capped).
> Keep the SG restricted to your IP and don't share the token.

**Logs / status files:**
- `mm_status.json` — written by `mm_gateway.py` each cycle (open count, target notional, markets).
- The MM gateway and the connector both **log every action to stdout** with timestamps
  (`re-quote`, `OK/ERR`, `hold`, `FLATTEN`, `cancel_all`). Redirect to a file with
  `... > logs/mm.log 2>&1 &` if you run it detached.
- C++ bot logs: per `config.live.json` `log_file` / summary lines (latency, fills, P&L).

---

## 8. Safety model (built in)

- **post-only** — every order; we never take, never pay taker fees.
- **cash-only** — only BUY orders; max loss is bounded by what we choose to buy.
- **Hard caps** — per-order notional, total notional, size, open-order count
  (enforced in both `mm_gateway.py` and the connector).
- **Cancel-all on start AND on every exit** — startup clears orphans; SIGINT/SIGTERM/
  normal exit all flatten. The connector also flattens on its own shutdown.
- **Dead-man's-switch** — if market data goes stale, the MM gateway cancels everything.
- **Arm latch** — `--live` (gateway) / `live_arm` (bot) default OFF = dry-run.
- **Secrets** — key + L2 secret are file-only (chmod 600, outside the repo), never
  logged, never committed. The connector token comes from the environment.

---

## 9. Funding & withdrawing

- **Funds live in the deposit wallet** `0x8323…`. To add more: deposit on
  polymarket.com (connect the EOA `0x4E3b…` in MetaMask → "instant transfer"),
  which moves pUSD in **and** sets allowances.
- **To withdraw:** use polymarket.com's withdraw flow (deposit wallet → your EOA),
  or unwrap pUSD → USDC. Nothing is locked — you control the owner key.

---

## 10. Rebuild / test after code changes

```bash
cmake --build build        # builds arb_detector (incl. relay mode)
./build/live_test          # 56 checks — must say ALL PASS
./build/test_executor      # strategy/OMS/ACR checks
```

---

## 11. Troubleshooting

| symptom | cause / fix |
|---|---|
| `not enough balance / allowance` | deposit wallet is empty or allowances unset → fund via polymarket.com |
| `maker address not allowed, use the deposit wallet flow` | wrong maker/sigType — must be `funder=0x8323…`, `signature_type=3` |
| `invalid POLY_1271 signature ... TypedDataSign` | order signed as plain ECDSA — must go through `py-clob-client-v2` (the connector) |
| connector `401 unauthorized` | `ORDER_GW_TOKEN` mismatch between connector and bot |
| `RELAY preflight FAIL` | connector not running on `relay_host:relay_port`, or wrong token |
| RPC `403 Forbidden` | add a browser `User-Agent` header to Polygon RPC calls |
| stale "open orders" right after cancel | eventual consistency — re-query; `cancel_all` returns the authoritative `canceled:[…]` |

---

## 12. File map (live path)

```
tools/mm_gateway.py            Python market-maker (mode A) — cash-only 2-sided, caps, DMS
tools/order_gateway_server.py  Python connector (mode B) — HTTP service wrapping the SDK
src/relay_gateway.hpp          C++ RelayGateway : IExecGateway — forwards to the connector
src/main.cpp / oms.hpp         exec_mode="relay" wiring
tools/probe_safe_maker.py      diagnostic: probe maker/sigType acceptance
tools/derive_api_key_1271.py   diagnostic: ERC-7739 login experiments
.pmlibs/                       py-clob-client-v2 (gitignored; PYTHONPATH=./.pmlibs)
/home/ubuntu/.pm_signer_key    EOA private key (0600, OUTSIDE repo)
/home/ubuntu/.pm_creds.env     L2 API creds (0600, OUTSIDE repo)
```
```bash
# (re)install the SDK locally if .pmlibs is missing:
python3 -m pip install --break-system-packages --target=./.pmlibs py-clob-client-v2
```
