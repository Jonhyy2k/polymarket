#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# run_live.sh — launch the bot on the real Polymarket CLOB v2 path.
#
#   DRY-RUN (default): signs + builds every order but POSTs NOTHING. Safe.
#       ./run_live.sh
#   REFRESH markets first (rotate daily — do this near launch):
#       ./run_live.sh --refresh
#   ARMED (real orders, real money — needs funded wallet + approvals + creds):
#       ./run_live.sh --arm
#
# Secrets come from the ENVIRONMENT only (never a file in the repo, never logged):
#   PM_SIGNER_KEY  PM_API_KEY  PM_API_SECRET  PM_API_PASSPHRASE
#   (PM_RPC_URL only needed for the one-time on-chain setup tools, not here.)
# ─────────────────────────────────────────────────────────────────────────────
set -euo pipefail
cd "$(dirname "$0")"

CONFIG="config.live.json"
BIN="build/arb_detector"
ARM=false
REFRESH=false
for a in "$@"; do
  case "$a" in
    --arm)     ARM=true ;;
    --refresh) REFRESH=true ;;
    *) echo "unknown flag: $a"; exit 2 ;;
  esac
done

# 0) binary present?
[[ -x "$BIN" ]] || { echo "ERROR: $BIN not built. Run: cmake --build build"; exit 1; }

# 1) required secrets present? (we check presence only — never print them)
missing=()
for v in PM_SIGNER_KEY PM_API_KEY PM_API_SECRET PM_API_PASSPHRASE; do
  [[ -n "${!v:-}" ]] || missing+=("$v")
done
if (( ${#missing[@]} )); then
  echo "ERROR: missing env: ${missing[*]}"
  echo "  export PM_SIGNER_KEY=\"\$(cat /home/ubuntu/.pm_signer_key)\""
  echo "  then derive + export PM_API_KEY/PM_API_SECRET/PM_API_PASSPHRASE (tools/derive_api_key.py)"
  exit 1
fi

# 2) refresh the live market list (they rotate daily)
if $REFRESH; then
  echo "[run_live] refreshing active markets -> $CONFIG"
  python3 tools/build_live_config.py "$CONFIG" 16
fi
[[ -f "$CONFIG" ]] || { echo "ERROR: $CONFIG missing — run with --refresh first"; exit 1; }

# 3) derive the trading address from the key (never prints the key)
ADDR=$(PM_SIGNER_KEY="$PM_SIGNER_KEY" python3 - <<'PY'
import os
from eth_account import Account
print(Account.from_key(os.environ["PM_SIGNER_KEY"]).address)
PY
)

# 4) inject the live-exec + safety block (conservative caps for a ~$20 first run)
ARM_JSON=$([[ "$ARM" == true ]] && echo true || echo false)
ADDR="$ADDR" ARM_JSON="$ARM_JSON" CONFIG="$CONFIG" python3 - <<'PY'
import json, os
cfg = json.load(open(os.environ["CONFIG"]))
addr = os.environ["ADDR"]; arm = os.environ["ARM_JSON"] == "true"
cfg.update({
    "shadow_executor_enabled": True,    # gates the whole exec/sender path
    "exec_mode": "live",
    "live_arm": arm,                    # false => sign but DO NOT POST
    "live_order_version": 2,
    "live_cancel_all_on_start": True,
    "live_signer_address": addr,
    "live_maker_address": addr,
    "live_signature_type": 0,           # EOA
    "live_order_type": "GTC",
    # conservative risk caps for the first live capital
    "risk_max_gross_notional_usd": 25.0,
    "risk_max_open_orders_total": 6,
    "risk_max_position_shares": 100.0,
    "dead_mans_switch_seconds": 30,     # flatten if the feed goes stale
    "shadow_executor_verbose": True,    # log each CREATE/CANCEL
})
json.dump(cfg, open(os.environ["CONFIG"], "w"), indent=4)
PY

# 5) loud banner + launch
echo "────────────────────────────────────────────────────────────"
echo " trading address : $ADDR"
echo " config          : $CONFIG  (contracts: $(python3 -c "import json;print(len(json.load(open('$CONFIG'))['contracts']))"))"
if $ARM; then
  echo " mode            : *** ARMED — REAL ORDERS, REAL MONEY ***"
else
  echo " mode            : DRY-RUN — orders signed but NOT sent (safe)"
fi
echo "────────────────────────────────────────────────────────────"
if $ARM; then
  read -r -p "Type ARM to send real orders: " ok
  [[ "$ok" == "ARM" ]] || { echo "aborted."; exit 1; }
fi

exec "$BIN" --config "$CONFIG"
