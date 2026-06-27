#!/usr/bin/env python3
"""
One-time on-chain allowances so the Polymarket v2 exchanges can move your funds:
  • ERC20 approve(collateral -> exchange)         for each exchange + neg-risk adapter
  • CTF   setApprovalForAll(exchange, true)        for each exchange + neg-risk adapter

The collateral token is READ FROM CHAIN (getCollateral() on the standard exchange)
so you never approve the wrong token — override with --collateral if you must.

SAFETY: dry-run by default — prints every tx (to / function / args / calldata) and
sends NOTHING. Add --send to broadcast (needs a little POL for gas). The EOA key is
read from $PM_SIGNER_KEY (or --keyfile) and never printed/logged.

    export PM_SIGNER_KEY=0x...           # the fresh trading EOA
    export PM_RPC_URL=https://polygon-mainnet.g.alchemy.com/v2/<KEY>
    python3 tools/approve_allowances.py            # dry run (review)
    python3 tools/approve_allowances.py --send     # broadcast
"""
import argparse
import json
import os
import sys
import urllib.request

from eth_account import Account
from eth_utils import keccak

CHAIN_ID = 137
CTF = "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045"
NEG_RISK_ADAPTER = "0xd91E80cF2E7be2e162c6513ceD06f1dD0dA35296"
EXCHANGE_STD = "0xE111180000d2663C0091e4f400237545B87B996B"
EXCHANGE_NEG = "0xe2222d279d744050d28e00520010520000310F59"
MAX_UINT = (1 << 256) - 1


def sel(sig: str) -> bytes:
    return keccak(text=sig)[:4]


def enc_addr(a: str) -> bytes:
    return bytes(12) + bytes.fromhex(a[2:] if a.startswith("0x") else a)


def enc_uint(n: int) -> bytes:
    return n.to_bytes(32, "big")


def approve_calldata(spender: str) -> str:
    return "0x" + (sel("approve(address,uint256)") + enc_addr(spender) + enc_uint(MAX_UINT)).hex()


def set_approval_calldata(operator: str) -> str:
    return "0x" + (sel("setApprovalForAll(address,bool)") + enc_addr(operator) + enc_uint(1)).hex()


def rpc_call(rpc: str, method: str, params: list):
    payload = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    req = urllib.request.Request(
        rpc, data=json.dumps(payload).encode(), method="POST",
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=20) as r:
        body = json.loads(r.read())
    if "error" in body:
        raise RuntimeError(body["error"])
    return body["result"]


def get_collateral(rpc: str) -> str:
    data = "0x" + sel("getCollateral()").hex()
    res = rpc_call(rpc, "eth_call", [{"to": EXCHANGE_STD, "data": data}, "latest"])
    if not res or len(res) < 66:
        raise RuntimeError("getCollateral() returned empty")
    return "0x" + res[-40:]


def load_key(args):
    if args.keyfile:
        with open(args.keyfile) as f:
            return f.read().strip()
    k = os.environ.get("PM_SIGNER_KEY", "").strip()
    if not k:
        print("ERROR: set $PM_SIGNER_KEY or pass --keyfile", file=sys.stderr)
        sys.exit(2)
    return k


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default=os.environ.get("PM_RPC_URL", ""))
    ap.add_argument("--keyfile")
    ap.add_argument("--collateral", help="override collateral token (else read on-chain)")
    ap.add_argument("--send", action="store_true", help="broadcast (default: dry run)")
    args = ap.parse_args()
    if not args.rpc:
        print("ERROR: pass --rpc or set PM_RPC_URL", file=sys.stderr)
        return 2

    key = load_key(args)
    acct = Account.from_key(key)
    print(f"signer  : {acct.address}")

    collateral = args.collateral or get_collateral(args.rpc)
    print(f"collateral (on-chain getCollateral): {collateral}")
    print(f"CTF     : {CTF}\n")

    # The full approval set Polymarket uses (exchanges + neg-risk adapter).
    spenders = [("exchange std", EXCHANGE_STD),
                ("exchange neg", EXCHANGE_NEG),
                ("neg-risk adapter", NEG_RISK_ADAPTER)]
    txs = []
    for label, spender in spenders:
        txs.append((f"USDC.approve({label})", collateral, approve_calldata(spender)))
    for label, operator in spenders:
        txs.append((f"CTF.setApprovalForAll({label})", CTF, set_approval_calldata(operator)))

    if not args.send:
        print("DRY RUN — nothing sent. Review, then re-run with --send:\n")
        for name, to, data in txs:
            print(f"  {name}\n     to   = {to}\n     data = {data}\n")
        print(f"{len(txs)} transactions queued. Est. gas: a few cents of POL total.")
        return 0

    nonce = int(rpc_call(args.rpc, "eth_getTransactionCount", [acct.address, "pending"]), 16)
    gas_price = int(rpc_call(args.rpc, "eth_gasPrice", []), 16)
    gas_price = int(gas_price * 1.25)  # headroom for Polygon spikes
    print(f"start nonce={nonce}  gasPrice={gas_price/1e9:.1f} gwei  — broadcasting {len(txs)} txs\n")

    for name, to, data in txs:
        tx = {
            "to": to, "data": data, "value": 0, "nonce": nonce,
            "chainId": CHAIN_ID, "gas": 120000, "gasPrice": gas_price,
        }
        try:
            est = int(rpc_call(args.rpc, "eth_estimateGas",
                               [{"from": acct.address, "to": to, "data": data}]), 16)
            tx["gas"] = int(est * 1.3)
        except Exception as e:
            print(f"  {name}: gas estimate failed ({e}); using 120000")
        signed = acct.sign_transaction(tx)
        raw = signed.raw_transaction.hex()
        raw = raw if raw.startswith("0x") else "0x" + raw
        txh = rpc_call(args.rpc, "eth_sendRawTransaction", [raw])
        print(f"  {name}: sent nonce={nonce} tx={txh}")
        nonce += 1
    print("\nAll approvals broadcast. Wait for confirmations, then run the bot live.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
