#!/usr/bin/env python3
"""
Wrap USDC -> pUSD via Polymarket's CollateralOnramp, so a direct-EOA trader holds
the collateral the v2 exchange actually settles in.

The v2 exchange's getCollateral() is **pUSD** (0xC011a7…, "Polymarket USD", 6dp),
backed 1:1 by USDC. API traders must wrap USDC into pUSD themselves:
    1) ERC20 approve(asset -> CollateralOnramp, amount)
    2) CollateralOnramp.wrap(asset, recipient, amount)   # selector 0x62355638 (verified on-chain)

Polymarket docs say the onramp accepts **bridged USDC.e** (0x2791…), not native
USDC (0x3c49…). The accepted-asset list is in contract storage (not hardcoded),
so if you hold native USDC, FIRST confirm it's accepted with a tiny --send test
(or swap native->USDC.e on a DEX). Override the asset with --asset.

SAFETY: dry-run by default (prints calldata, sends nothing). --send broadcasts
(needs POL for gas). Key from $PM_SIGNER_KEY/--keyfile (never printed).

    export PM_SIGNER_KEY="$(cat /home/ubuntu/.pm_signer_key)"
    export PM_RPC_URL=https://polygon-mainnet.g.alchemy.com/v2/<KEY>
    python3 tools/wrap_pusd.py --amount 25            # dry run (USDC.e, 25.0)
    python3 tools/wrap_pusd.py --amount 1 --asset native --send   # tiny native test
"""
import argparse
import json
import os
import sys
import time
import urllib.request

from eth_account import Account
from eth_utils import keccak, to_checksum_address

CHAIN_ID = 137
ONRAMP = "0x93070a847efEf7F70739046A929D47a521F5B8ee"
PUSD = "0xC011a7E12a19f7B1f670d46F03B03f3342E82DFB"
USDC_E = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"
USDC_NATIVE = "0x3c499c542cEF5E3811e1192ce70d8cc03d5c3359"
MAX_UINT = (1 << 256) - 1


def sel(sig: str) -> bytes:
    return keccak(text=sig)[:4]


def enc_addr(a: str) -> bytes:
    return bytes(12) + bytes.fromhex(a[2:])


def enc_uint(n: int) -> bytes:
    return n.to_bytes(32, "big")


def rpc_call(rpc, method, params):
    p = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    req = urllib.request.Request(rpc, data=json.dumps(p).encode(), method="POST",
                                 headers={"Content-Type": "application/json",
                                          "User-Agent": "Mozilla/5.0"})
    body = json.loads(urllib.request.urlopen(req, timeout=20).read())
    if "error" in body:
        raise RuntimeError(body["error"])
    return body["result"]


def load_key(args):
    if args.keyfile:
        return open(args.keyfile).read().strip()
    k = os.environ.get("PM_SIGNER_KEY", "").strip()
    if not k:
        print("ERROR: set $PM_SIGNER_KEY or --keyfile", file=sys.stderr); sys.exit(2)
    return k


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default=os.environ.get("PM_RPC_URL", ""))
    ap.add_argument("--keyfile")
    ap.add_argument("--amount", type=float, required=True, help="USDC amount (human, e.g. 25)")
    ap.add_argument("--asset", default="usdce", choices=["usdce", "native"],
                    help="which USDC to wrap (default usdce per docs)")
    ap.add_argument("--to", help="pUSD recipient (default: signer)")
    ap.add_argument("--send", action="store_true")
    args = ap.parse_args()
    if not args.rpc:
        print("ERROR: pass --rpc or set PM_RPC_URL", file=sys.stderr); return 2

    acct = Account.from_key(load_key(args))
    asset = USDC_E if args.asset == "usdce" else USDC_NATIVE
    to = args.to or acct.address
    amount = int(round(args.amount * 1_000_000))  # 6 decimals

    approve_data = "0x" + (sel("approve(address,uint256)") + enc_addr(ONRAMP) + enc_uint(MAX_UINT)).hex()
    wrap_data = "0x" + (sel("wrap(address,address,uint256)") +
                        enc_addr(asset) + enc_addr(to) + enc_uint(amount)).hex()

    print(f"signer   : {acct.address}")
    print(f"asset    : {asset}  ({args.asset})")
    print(f"amount   : {args.amount}  ({amount} base units)")
    print(f"recipient: {to}")
    print(f"onramp   : {ONRAMP}   pUSD: {PUSD}\n")
    txs = [(f"approve({args.asset} -> onramp)", asset, approve_data),
           (f"wrap({args.asset} {args.amount} -> pUSD)", ONRAMP, wrap_data)]

    if not args.send:
        print("DRY RUN — nothing sent. Review, then --send (needs POL for gas):\n")
        for name, t, d in txs:
            print(f"  {name}\n     to   = {t}\n     data = {d}\n")
        print("NOTE: if --asset native reverts on wrap, native USDC isn't accepted —")
        print("      swap native->USDC.e on a DEX, or re-fund as USDC.e.")
        return 0

    nonce = int(rpc_call(args.rpc, "eth_getTransactionCount", [acct.address, "pending"]), 16)
    gas_price = int(int(rpc_call(args.rpc, "eth_gasPrice", []), 16) * 1.25)
    for name, to_, data in txs:
        to_ = to_checksum_address(to_)
        tx = {"to": to_, "data": data, "value": 0, "nonce": nonce,
              "chainId": CHAIN_ID, "gas": 250000, "gasPrice": gas_price}
        try:
            est = int(rpc_call(args.rpc, "eth_estimateGas",
                               [{"from": acct.address, "to": to_, "data": data}]), 16)
            tx["gas"] = int(est * 1.3)
        except Exception as e:
            print(f"  {name}: gas estimate FAILED ({e}) — likely a revert; stopping.")
            return 1
        signed = acct.sign_transaction(tx)
        raw = signed.raw_transaction.hex()
        raw = raw if raw.startswith("0x") else "0x" + raw
        txh = rpc_call(args.rpc, "eth_sendRawTransaction", [raw])
        print(f"  {name}: sent nonce={nonce} tx={txh}  (waiting...)")
        nonce += 1
        for _ in range(60):
            time.sleep(2)
            rcpt = rpc_call(args.rpc, "eth_getTransactionReceipt", [txh])
            if rcpt:
                ok = int(rcpt.get("status", "0x0"), 16) == 1
                print(f"       mined: {'OK' if ok else 'REVERTED'} block={int(rcpt['blockNumber'],16)}")
                if not ok:
                    return 1
                break
        else:
            print("       timeout waiting for receipt."); return 1
    print("\nWrap broadcast + mined. Check pUSD balance, then approve pUSD to the exchanges.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
