#!/usr/bin/env python3
"""
Resolve the REAL collateral + CTF token of the live Polymarket v2 exchanges,
on-chain — so allowances are approved to the right token (never a guess).

The @polymarket/clob-client-v2 config.ts lists collateral 0xC011a7E1… (identical
for testnet+mainnet, i.e. a placeholder), so we read it straight off the deployed
Exchange instead: getCollateral() and getCtf() on each v2 exchange contract.

Read-only (eth_call). Needs an authenticated Polygon RPC (public ones are blocked
from the trading box):
    python3 tools/verify_collateral.py --rpc https://polygon-mainnet.g.alchemy.com/v2/<KEY>
    PM_RPC_URL=... python3 tools/verify_collateral.py
"""
import argparse
import json
import os
import sys
import urllib.request

from eth_utils import keccak

CHAIN_ID = 137
EXCHANGES = {
    "v2 standard": "0xE111180000d2663C0091e4f400237545B87B996B",
    "v2 neg-risk": "0xe2222d279d744050d28e00520010520000310F59",
    "v3 unified":  "0xe3333700cA9d93003F00f0F71f8515005F6c00Aa",
}
KNOWN = {
    "0x2791bca1f2de4661ed88a30c99a7a9449aa84174": "USDC.e (bridged) — V1 collateral",
    "0x3c499c542cef5e3811e1192ce70d8cc03d5c3359": "native USDC",
    "0xc011a7e12a19f7b1f670d46f03b03f3342e82dfb": "the repo placeholder (NOT real)",
    "0x4d97dcd97ec945f40cf65f87097ace5ea0476045": "CTF (ConditionalTokens)",
}


def selector(sig: str) -> str:
    return "0x" + keccak(text=sig)[:4].hex()


def eth_call(rpc: str, to: str, data: str):
    payload = {
        "jsonrpc": "2.0", "id": 1, "method": "eth_call",
        "params": [{"to": to, "data": data}, "latest"],
    }
    req = urllib.request.Request(
        rpc, data=json.dumps(payload).encode(), method="POST",
        headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=15) as r:
        body = json.loads(r.read())
    if "error" in body:
        return None, body["error"].get("message", str(body["error"]))
    res = body.get("result", "0x")
    if not res or res == "0x" or len(res) < 66:
        return None, "empty result (no such method on this contract?)"
    return "0x" + res[-40:], None  # last 20 bytes = address


def label(addr: str) -> str:
    return KNOWN.get(addr.lower(), "unknown — verify before approving")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default=os.environ.get("PM_RPC_URL", ""))
    args = ap.parse_args()
    if not args.rpc:
        print("ERROR: pass --rpc <polygon-url> or set PM_RPC_URL", file=sys.stderr)
        return 2

    sel_coll = selector("getCollateral()")
    sel_ctf = selector("getCtf()")
    print(f"getCollateral() selector = {sel_coll}   getCtf() selector = {sel_ctf}\n")

    seen_collateral = set()
    for name, addr in EXCHANGES.items():
        print(f"[{name}] {addr}")
        coll, err = eth_call(args.rpc, addr, sel_coll)
        if err:
            print(f"   getCollateral() -> ERROR: {err}")
        else:
            print(f"   collateral = {coll}   ({label(coll)})")
            seen_collateral.add(coll.lower())
        ctf, err = eth_call(args.rpc, addr, sel_ctf)
        if err:
            print(f"   getCtf()        -> ERROR: {err}")
        else:
            print(f"   ctf        = {ctf}   ({label(ctf)})")
        print()

    if len(seen_collateral) == 1:
        c = next(iter(seen_collateral))
        print(f"✅ All exchanges share collateral {c} ({label(c)}).")
        print("   Approve THIS token (not the repo placeholder) to the exchanges.")
    elif seen_collateral:
        print("⚠️  Exchanges report DIFFERENT collateral tokens — approve each accordingly:")
        for c in seen_collateral:
            print(f"     {c}  ({label(c)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
