#!/usr/bin/env python3
"""
Swap native USDC -> bridged USDC.e via Uniswap v3 (the onramp only accepts USDC.e).

Uses the canonical Uniswap v3 SwapRouter exactInputSingle on the deep 0.01% pool.
A hard amountOutMinimum floor protects against any bad fill; eth_estimateGas runs
before broadcast and aborts if the call would revert (encoding/allowance safety).

SAFETY: dry-run by default. --send broadcasts (approve + swap). Key from
$PM_SIGNER_KEY/--keyfile, never printed.

    python3 tools/swap_usdc.py --amount 25 --min-out 24.5            # dry run
    python3 tools/swap_usdc.py --amount 25 --min-out 24.5 --send
"""
import argparse, json, os, sys, time, urllib.request
from eth_account import Account
from eth_utils import keccak, to_checksum_address

CHAIN_ID = 137
ROUTER = "0xE592427A0AEce92De3Edee1F18E0157C05861564"   # Uniswap v3 SwapRouter
NATIVE = "0x3c499c542cEF5E3811e1192ce70d8cc03d5c3359"
USDCE  = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174"
MAX_UINT = (1 << 256) - 1


def sel(sig): return keccak(text=sig)[:4]
def enc_addr(a): return bytes(12) + bytes.fromhex(a[2:])
def enc_uint(n): return n.to_bytes(32, "big")


def rpc(url, method, params):
    p = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    req = urllib.request.Request(url, data=json.dumps(p).encode(), method="POST",
                                 headers={"Content-Type": "application/json", "User-Agent": "Mozilla/5.0"})
    body = json.loads(urllib.request.urlopen(req, timeout=20).read())
    if "error" in body:
        raise RuntimeError(body["error"])
    return body["result"]


def load_key(args):
    if args.keyfile: return open(args.keyfile).read().strip()
    k = os.environ.get("PM_SIGNER_KEY", "").strip()
    if not k: print("ERROR: set $PM_SIGNER_KEY or --keyfile", file=sys.stderr); sys.exit(2)
    return k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default=os.environ.get("PM_RPC_URL", "https://polygon-bor-rpc.publicnode.com"))
    ap.add_argument("--keyfile")
    ap.add_argument("--amount", type=float, required=True)
    ap.add_argument("--min-out", type=float, required=True)
    ap.add_argument("--fee", type=int, default=100)   # 0.01% pool
    ap.add_argument("--send", action="store_true")
    args = ap.parse_args()

    acct = Account.from_key(load_key(args))
    amount_in = int(round(args.amount * 1e6))
    min_out = int(round(args.min_out * 1e6))
    deadline = int(time.time()) + 1200

    approve_data = "0x" + (sel("approve(address,uint256)") + enc_addr(ROUTER) + enc_uint(MAX_UINT)).hex()
    # exactInputSingle((tokenIn,tokenOut,fee,recipient,deadline,amountIn,amountOutMinimum,sqrtPriceLimitX96))
    swap = (sel("exactInputSingle((address,address,uint24,address,uint256,uint256,uint256,uint160))")
            + enc_addr(NATIVE) + enc_addr(USDCE) + enc_uint(args.fee) + enc_addr(acct.address)
            + enc_uint(deadline) + enc_uint(amount_in) + enc_uint(min_out) + enc_uint(0))
    swap_data = "0x" + swap.hex()

    print(f"signer  : {acct.address}")
    print(f"swap    : {args.amount} native USDC -> >= {args.min_out} USDC.e (fee {args.fee/10000:.2f}%)")
    txs = [("approve(native -> router)", NATIVE, approve_data),
           ("exactInputSingle(native -> USDC.e)", ROUTER, swap_data)]

    if not args.send:
        print("\nDRY RUN — nothing sent:\n")
        for n, to, d in txs:
            print(f"  {n}\n     to   = {to}\n     data = {d}\n")
        return 0

    nonce = int(rpc(args.rpc, "eth_getTransactionCount", [acct.address, "pending"]), 16)
    gas_price = int(int(rpc(args.rpc, "eth_gasPrice", []), 16) * 1.3)
    for n, to, data in txs:
        to = to_checksum_address(to)
        tx = {"to": to, "data": data, "value": 0, "nonce": nonce,
              "chainId": CHAIN_ID, "gas": 300000, "gasPrice": gas_price}
        try:
            est = int(rpc(args.rpc, "eth_estimateGas", [{"from": acct.address, "to": to, "data": data}]), 16)
            tx["gas"] = int(est * 1.3)
        except Exception as e:
            print(f"  {n}: estimateGas FAILED ({e}) — would revert; aborting before broadcast.")
            return 1
        signed = acct.sign_transaction(tx)
        raw = signed.raw_transaction.hex()
        raw = raw if raw.startswith("0x") else "0x" + raw
        txh = rpc(args.rpc, "eth_sendRawTransaction", [raw])
        print(f"  {n}: sent nonce={nonce} tx={txh}  (waiting for receipt...)")
        nonce += 1
        # wait for mining so the next tx's estimateGas sees this state (e.g. the
        # swap needs the approve confirmed). ~2s blocks on Polygon.
        for _ in range(60):
            time.sleep(2)
            rcpt = rpc(args.rpc, "eth_getTransactionReceipt", [txh])
            if rcpt:
                ok = int(rcpt.get("status", "0x0"), 16) == 1
                print(f"       mined: {'OK' if ok else 'REVERTED'} block={int(rcpt['blockNumber'],16)}")
                if not ok:
                    print("       aborting — tx reverted on-chain.")
                    return 1
                break
        else:
            print("       timeout waiting for receipt; check explorer before retrying.")
            return 1
    print("\nSwap broadcast + mined. Check USDC.e balance.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
