# Going live (CLOB v2) — runbook

The trading code is built and verified offline + in dry-run. What remains is
money/ops. **[YOU]** = you run it (needs your key/funds). **[ME]** = already done
or a helper script.

## The wallet
A fresh, dedicated EOA holds the trading capital and signs orders. Created on this
box (key never leaves it — written to a `0600` file, only the address printed):

```
python3 tools/new_wallet.py /home/ubuntu/.pm_signer_key   # [ME] done
```

Fund **this address** (printed by the tool) on **Polygon**, never your main wallet.

## Steps

1. **[YOU] Verify the collateral token** (so you fund/approve the right one):
   ```
   python3 tools/verify_collateral.py --rpc <your-alchemy-polygon-url>
   ```
2. **[YOU] Fund the wallet** on Polygon: ~$20 of that collateral token + ~$1 POL (gas).
3. **[YOU] Approve allowances** (review dry-run, then send — signs with your key, spends gas):
   ```
   export PM_SIGNER_KEY="$(cat /home/ubuntu/.pm_signer_key)"
   export PM_RPC_URL=<your-alchemy-polygon-url>
   python3 tools/approve_allowances.py            # review
   python3 tools/approve_allowances.py --send     # broadcast
   ```
4. **[YOU] Derive L2 API creds** and export the secrets in your launch shell:
   ```
   python3 tools/derive_api_key.py
   export PM_SIGNER_KEY=...  PM_API_KEY=...  PM_API_SECRET=...  PM_API_PASSPHRASE=...
   ```
5. **[YOU] Dry-run on the live feed** (signs but POSTs nothing — safe):
   ```
   ./run_live.sh --refresh        # refresh markets, dry-run
   ```
   Confirm the log shows `[LIVE] preflight: PASS … /version=2` and `[LIVE-DRY] CREATE …`.
6. **[YOU] Armed probe, then real** (real orders, real money):
   ```
   ./run_live.sh --arm            # type ARM to confirm
   ```

## Safety model (built in)
- `live_arm=false` (default) = **dry-run**: orders are signed + built but never POSTed.
- Secrets are **env-only** (`PM_SIGNER_KEY`, `PM_API_KEY/SECRET/PASSPHRASE`) — never in
  config, never logged, never committed.
- `preflight()` refuses to arm unless keccak is correct, creds+key are present,
  `address(key)==signer`, and the **live `GET /version` == 2** (what the binary builds).
- Startup `cancel-all` clears any orphaned resting orders before quoting.
- `run_live.sh` sets conservative caps for the first run: gross ≤ $25, ≤ 4 open orders,
  ≤ 100 shares/position, 30s dead-man's-switch flatten.

## Live config fields (in `config.*.json`; `run_live.sh` sets these for you)
| field | meaning |
|---|---|
| `exec_mode` | `"live"` to use the real path (`"shadow"`/`"mocklive"` otherwise) |
| `shadow_executor_enabled` | must be `true` — gates the exec/sender thread |
| `live_arm` | `false`=dry-run (no POST), `true`=send real orders |
| `live_order_version` | expected CLOB version (preflight checks `/version`); currently `2` |
| `live_signer_address`/`live_maker_address` | the trading EOA (auto-filled from the key) |
| `live_signature_type` | `0`=EOA |
| `live_cancel_all_on_start` | clear orphaned orders at startup |
| `live_host`/`live_port` | CLOB host (default `clob.polymarket.com:443`) |
