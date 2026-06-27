# Polymarket bot — operator instructions

Everything you need to run, monitor, and understand the current state. Last
updated 2026-06-27.

---

## TL;DR status
- **Engineering: done & verified.** Auth, V2 EIP-712 signing, order format,
  transport, on-chain pUSD + approvals — all correct and proven against
  production (the armed order reached Polymarket's matching engine).
- **Funds: safe.** Trading wallet holds **~24.997 pUSD + ~5.49 POL**. Nothing was
  lost — see "the blocker" below.
- **🚨 Blocker: Polymarket V2 won't let a bare EOA trade.** The armed run was
  rejected with `maker address not allowed, please use the deposit wallet flow`.
  We need a Polymarket **proxy/deposit wallet** (see "Next step"). Until then,
  live orders cannot rest.

---

## Key facts
| thing | value |
|---|---|
| Trading EOA (signer) | `0x4E3b143938947039b2F0b13BD1038683DE57851F` |
| Private key file | `/home/ubuntu/.pm_signer_key` (chmod 600 — never share/commit) |
| API creds file | `/home/ubuntu/.pm_creds.env` (chmod 600 — PM_API_* + PM_SIGNER_KEY) |
| Collateral token | pUSD `0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB` (6 decimals) |
| Exchanges (v2) | std `0xE111180000d2663C0091e4f400237545B87B996B`, neg `0xe2222d279d744050d28e00520010520000310F59` |
| Polygon RPC (reads) | `https://polygon-bor-rpc.publicnode.com` (needs a browser User-Agent) |
| CLOB host | `https://clob.polymarket.com` (production order version = 2) |

> The bot loads its config via **`--config <path>`** (a positional path is
> ignored and it falls back to `config.json`).

---

## Everyday commands

### Check wallet balances
```bash
python3 - <<'PY'
import json, urllib.request
RPC="https://polygon-bor-rpc.publicnode.com"; A="0x4E3b143938947039b2F0b13BD1038683DE57851F"
def rpc(m,p):
    r=urllib.request.Request(RPC,data=json.dumps({"jsonrpc":"2.0","id":1,"method":m,"params":p}).encode(),
        method="POST",headers={"Content-Type":"application/json","User-Agent":"Mozilla/5.0"})
    return json.loads(urllib.request.urlopen(r,timeout=12).read())["result"]
def bal(t):
    r=rpc("eth_call",[{"to":t,"data":"0x70a08231"+"0"*24+A[2:].lower()},"latest"]); return int(r,16)/1e6
print("pUSD:", bal("0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB"))
print("POL :", int(rpc("eth_getBalance",[A,"latest"]),16)/1e18)
PY
```

### Load secrets into your shell (needed before running the bot)
```bash
source /home/ubuntu/.pm_creds.env     # sets PM_SIGNER_KEY + PM_API_KEY/SECRET/PASSPHRASE
```

### Run the bot
```bash
cd /home/ubuntu/polymarket
source /home/ubuntu/.pm_creds.env

./run_live.sh --refresh        # DRY-RUN on fresh crypto markets (signs, sends NOTHING) — safe
./run_live.sh                  # DRY-RUN on whatever is in config.live.json
./run_live.sh --arm            # ARMED: real orders (type ARM to confirm). Blocked until deposit wallet exists.
```
- `--refresh` regenerates the market list (markets rotate daily). Omit it to keep
  the current `config.live.json` (e.g. the hand-picked reward markets).
- Dry-run is always safe: `live_arm=false` builds + signs but never POSTs.

### Stop the bot
```bash
pkill -f arb_detector
```

### Rebuild after code changes
```bash
cmake --build build            # produces build/arb_detector
./build/live_test              # 56 checks — must say ALL PASS
```

---

## One-time setup tools (already done — for reference)
| tool | what it does |
|---|---|
| `tools/new_wallet.py <file>` | generate a fresh EOA (key to 0600 file, prints address only) |
| `tools/verify_collateral.py --rpc <url>` | read the exchange's real collateral token on-chain |
| `tools/swap_usdc.py --amount N --min-out M --send` | native USDC → USDC.e (Uniswap v3) |
| `tools/wrap_pusd.py --rpc <url> --amount N --send` | USDC.e → pUSD (CollateralOnramp) |
| `tools/approve_allowances.py --rpc <url> --send` | approve pUSD + CTF to the exchanges |
| `tools/derive_api_key.py --create` | derive L2 API creds (reads PM_SIGNER_KEY) |

All money tools are **dry-run by default**; add `--send` to broadcast. They read
the key from `$PM_SIGNER_KEY` or `--keyfile` and never print it.

---

## Dashboard (monitoring)
```bash
cd /home/ubuntu/polymarket/dashboard
./start.sh                     # starts on port 8080, prints the URL
```
Open `http://<EC2-public-ip>:8080` (the AWS security group must allow TCP 8080).
Shows wallet balances, configured markets + live books, bot status, and risk.

---

## 🚨 The blocker & the next step
**What happened:** the armed run signed and sent two real V2 orders; Polymarket
rejected both with `maker address not allowed, please use the deposit wallet
flow`. This is an *account-model* rule, not a code bug — our orders are valid.

**Why:** Polymarket V2 requires the maker to be a **deposit/proxy wallet**
(signatureType 1 = POLY_PROXY, or 2 = Gnosis Safe), not a bare EOA
(signatureType 0, the "trade direct" model we started with). Their POLY_1271
deposit-wallet API path currently has open bugs on Polymarket's side.

**To go live we need to:**
1. Create a Polymarket **proxy/deposit wallet** controlled by the EOA
   `0x4E3b…851F` (most reliable via polymarket.com onboarding; a factory route
   may also be possible).
2. Move the **pUSD into that proxy wallet** (funds currently sit on the EOA).
3. Small code change: set `live_maker_address` = proxy address and
   `live_signature_type` = 1 (our existing signer already handles sig types 1/2).

**Your funds are not stuck:** the pUSD/POL are on the EOA you control. We can move
them to the proxy once it exists, or unwrap pUSD → USDC anytime.

---

## Safety model (built in)
- `live_arm=false` (default) = dry-run; orders are signed but never sent.
- Secrets are env/file-only (0600), never in the repo, never logged.
- `preflight()` blocks arming unless keccak is correct, creds+key present,
  `address(key)==signer`, and live `GET /version == 2`.
- Conservative caps on the live run: gross ≤ $25, ≤ 6 open orders, ≤ 100
  shares/position, 30s dead-man's-switch flatten.
- Startup `cancel-all` clears any orphaned orders before quoting.
