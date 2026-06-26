#!/usr/bin/env python3
"""
L1 → L2: derive Polymarket CLOB API credentials from an EOA private key.

ONE-TIME setup. Signs the ClobAuth EIP-712 message with the EOA and calls the
CLOB /auth endpoints to obtain {api_key, secret, passphrase} — the L2 creds the
C++ bot uses (clob_auth.hpp) to authenticate every order request.

Deriving creds needs ONLY a signature — NO funds, NO allowances, NO on-chain tx.
That makes this the zero-cost end-to-end validation of signer + EIP-712 + auth +
transport against the live API.

Key handling: the private key is read from $PM_SIGNER_KEY (or --keyfile). It is
never printed, logged, or committed.

Usage:
    export PM_SIGNER_KEY=0x...            # or: --keyfile /path/to/keyfile
    python3 tools/derive_api_key.py
    python3 tools/derive_api_key.py --create   # create new instead of derive
"""
import argparse
import json
import os
import sys
import time
import urllib.request
import urllib.error

from eth_account import Account
from eth_account.messages import encode_typed_data

CLOB_HOST = "https://clob.polymarket.com"
CHAIN_ID = 137
CLOB_DOMAIN_NAME = "ClobAuthDomain"
CLOB_VERSION = "1"
MSG_TO_SIGN = "This message attests that I control the given wallet"


def clob_auth_signature(private_key: str, address: str, timestamp: int, nonce: int) -> str:
    """EIP-712 sign the ClobAuth struct. Domain has NO verifyingContract."""
    full_message = {
        "primaryType": "ClobAuth",
        "domain": {
            "name": CLOB_DOMAIN_NAME,
            "version": CLOB_VERSION,
            "chainId": CHAIN_ID,
        },
        "types": {
            "EIP712Domain": [
                {"name": "name", "type": "string"},
                {"name": "version", "type": "string"},
                {"name": "chainId", "type": "uint256"},
            ],
            "ClobAuth": [
                {"name": "address", "type": "address"},
                {"name": "timestamp", "type": "string"},
                {"name": "nonce", "type": "uint256"},
                {"name": "message", "type": "string"},
            ],
        },
        "message": {
            "address": address,
            "timestamp": str(timestamp),
            "nonce": nonce,
            "message": MSG_TO_SIGN,
        },
    }
    signable = encode_typed_data(full_message=full_message)
    signed = Account.sign_message(signable, private_key)
    sig = signed.signature.hex()
    return sig if sig.startswith("0x") else "0x" + sig


def l1_headers(address: str, signature: str, timestamp: int, nonce: int) -> dict:
    return {
        "POLY_ADDRESS": address,
        "POLY_SIGNATURE": signature,
        "POLY_TIMESTAMP": str(timestamp),
        "POLY_NONCE": str(nonce),
    }


def call(method: str, path: str, headers: dict):
    # Cloudflare in front of Polymarket bans the default Python-urllib UA (err 1010).
    # A normal UA + Accept clears it (same as the working C++/dashboard paths).
    headers = {
        **headers,
        "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) polymarket-lp/1.0",
        "Accept": "application/json",
    }
    req = urllib.request.Request(CLOB_HOST + path, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "ignore")
        try:
            body = json.loads(body)
        except Exception:
            pass
        return e.code, body


def load_key(args) -> str:
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
    ap.add_argument("--keyfile", help="path to a file containing the 0x private key")
    ap.add_argument("--create", action="store_true",
                    help="POST /auth/api-key (create new) instead of GET derive")
    ap.add_argument("--nonce", type=int, default=0)
    args = ap.parse_args()

    key = load_key(args)
    acct = Account.from_key(key)
    address = acct.address
    timestamp = int(time.time())

    sig = clob_auth_signature(key, address, timestamp, args.nonce)
    headers = l1_headers(address, sig, timestamp, args.nonce)

    print(f"signer address : {address}")
    print(f"L1 ClobAuth sig: {sig[:18]}…{sig[-6:]}  (timestamp={timestamp}, nonce={args.nonce})")

    if args.create:
        status, body = call("POST", "/auth/api-key", headers)
        label = "POST /auth/api-key (create)"
    else:
        status, body = call("GET", "/auth/derive-api-key", headers)
        label = "GET /auth/derive-api-key"
    print(f"{label} -> HTTP {status}")

    if status == 200 and isinstance(body, dict):
        api_key = body.get("apiKey") or body.get("api_key")
        secret = body.get("secret")
        passphrase = body.get("passphrase")
        if api_key and secret and passphrase:
            print("\n✅ L2 credentials obtained (signer + EIP-712 + auth + transport all work):")
            print(f"  PM_API_KEY={api_key}")
            print(f"  PM_API_SECRET={secret}")
            print(f"  PM_API_PASSPHRASE={passphrase}")
            print("\nStore these (env or config) for the C++ LiveGateway. Do not commit.")
            return 0
    print("\nResponse body:")
    print(json.dumps(body, indent=2) if isinstance(body, (dict, list)) else body)
    print("\n(If derive returned no creds, try --create once.)")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
