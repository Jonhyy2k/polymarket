#!/usr/bin/env python3
"""
FEASIBILITY TEST — register a CLOB L2 API key to a Polymarket *deposit wallet*
(Solady ERC-7739 smart wallet), signing with the owner EOA.

This is the make-or-break gate for the new deposit-wallet (POLY_1271) flow:
Polymarket's own SDKs can't do it (they bind the key to the EOA, not the wallet —
py-clob-client-v2 #70/#75). We control our own auth, so we sign the ClobAuth login
the way the wallet's isValidSignature() actually expects and ask the server to bind
the key to the DEPOSIT WALLET.

NO funds, NO allowances, NO on-chain tx, NO orders. Just a signature + an HTTP call.
If the server returns creds bound to the deposit wallet, the hard blocker is cleared.

Key handling: read from $PM_SIGNER_KEY or --keyfile; never printed/logged/committed.

How the wallet validates the login (Solady ERC-7739 "PersonalSign" path):
  hash        = EIP-712 digest of ClobAuth(address=DEPOSIT_WALLET, timestamp, nonce, message)
  structHash  = keccak256(PERSONAL_SIGN_TYPEHASH . hash)
  domainSep   = keccak256(EIP712Domain_TYPEHASH . k(name) . k(version) . chainId . wallet)
  digest      = keccak256(0x1901 . domainSep . structHash)
  POLY_SIGNATURE = ECDSA(digest) by the owner EOA   -> wallet.isValidSignature(hash, sig) == magic
"""
import argparse, json, os, sys, time, urllib.request, urllib.error
from eth_account import Account
from eth_account.messages import encode_typed_data
from eth_keys import keys
from eth_utils import keccak

CLOB_HOST = "https://clob.polymarket.com"
CHAIN_ID = 137
MSG_TO_SIGN = "This message attests that I control the given wallet"

# the deposit wallet (Solady ERC-7739) + its on-chain EIP-712 domain (read via eip712Domain())
WALLET = "0x832317706479bb6762741B9b9ba568bb86fFfFF0"
WALLET_DOMAIN_NAME = "DepositWallet"
WALLET_DOMAIN_VERSION = "1"

EIP712DOMAIN_TYPEHASH = keccak(
    b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")
PERSONAL_SIGN_TYPEHASH = keccak(b"PersonalSign(bytes prefixed)")


def k32_addr(a: str) -> bytes:
    return bytes(12) + bytes.fromhex(a[2:])


def clob_auth_digest(address: str, timestamp: int, nonce: int) -> bytes:
    typed = {
        "primaryType": "ClobAuth",
        "domain": {"name": "ClobAuthDomain", "version": "1", "chainId": CHAIN_ID},
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
        "message": {"address": address, "timestamp": str(timestamp),
                    "nonce": nonce, "message": MSG_TO_SIGN},
    }
    sm = encode_typed_data(full_message=typed)
    return keccak(b"\x19\x01" + sm.header + sm.body)  # header=domainSep, body=structHash


def wallet_domain_separator() -> bytes:
    return keccak(
        EIP712DOMAIN_TYPEHASH
        + keccak(WALLET_DOMAIN_NAME.encode())
        + keccak(WALLET_DOMAIN_VERSION.encode())
        + CHAIN_ID.to_bytes(32, "big")
        + k32_addr(WALLET))


def personal_sign_digest(inner_hash: bytes) -> bytes:
    struct_hash = keccak(PERSONAL_SIGN_TYPEHASH + inner_hash)
    return keccak(b"\x19\x01" + wallet_domain_separator() + struct_hash)


def ecdsa(privkey_hex: str, digest: bytes) -> str:
    pk = keys.PrivateKey(bytes.fromhex(privkey_hex[2:] if privkey_hex.startswith("0x") else privkey_hex))
    s = pk.sign_msg_hash(digest)
    return "0x" + bytes([*s.r.to_bytes(32, "big"), *s.s.to_bytes(32, "big"), s.v + 27]).hex()


def call(method: str, path: str, headers: dict):
    headers = {**headers,
               "User-Agent": "Mozilla/5.0 (X11; Linux x86_64) polymarket-lp/1.0",
               "Accept": "application/json"}
    req = urllib.request.Request(CLOB_HOST + path, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=12) as r:
            return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "ignore")
        try:
            body = json.loads(body)
        except Exception:
            pass
        return e.code, body


def attempt(label: str, address: str, signature: str, timestamp: int, nonce: int):
    headers = {"POLY_ADDRESS": address, "POLY_SIGNATURE": signature,
               "POLY_TIMESTAMP": str(timestamp), "POLY_NONCE": str(nonce)}
    print(f"\n=== {label} ===")
    print(f"  POLY_ADDRESS = {address}")
    print(f"  POLY_SIGNATURE = {signature[:20]}…{signature[-6:]}")
    for method, path in (("POST", "/auth/api-key"), ("GET", "/auth/derive-api-key")):
        status, body = call(method, path, headers)
        ok = status == 200 and isinstance(body, dict) and (body.get("apiKey") or body.get("api_key"))
        print(f"  {method} {path} -> HTTP {status}" + ("  ✅ CREDS!" if ok else ""))
        if ok:
            print(json.dumps(body, indent=2))
            return body
        else:
            print(f"      body: {json.dumps(body) if isinstance(body,(dict,list)) else body}")
    return None


def load_key(args) -> str:
    if args.keyfile:
        return open(args.keyfile).read().strip()
    k = os.environ.get("PM_SIGNER_KEY", "").strip()
    if not k:
        print("ERROR: set $PM_SIGNER_KEY or pass --keyfile", file=sys.stderr); sys.exit(2)
    return k


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--keyfile")
    ap.add_argument("--nonce", type=int, default=0)
    args = ap.parse_args()

    key = load_key(args)
    eoa = Account.from_key(key).address
    ts = int(time.time())
    print(f"owner EOA      : {eoa}")
    print(f"deposit wallet : {WALLET}")
    print(f"timestamp/nonce: {ts}/{args.nonce}")

    # ClobAuth digest with address = DEPOSIT WALLET (server reconstructs from POLY_ADDRESS)
    inner = clob_auth_digest(WALLET, ts, args.nonce)

    # Candidate 1: ERC-7739 PersonalSign wrap (what a Solady wallet's isValidSignature expects)
    sig_personal = ecdsa(key, personal_sign_digest(inner))
    if attempt("Candidate 1 — ERC-7739 PersonalSign (wallet as POLY_ADDRESS)",
               WALLET, sig_personal, ts, args.nonce):
        return 0

    # Candidate 2: plain ECDSA over the ClobAuth digest (diagnostic — likely rejected)
    sig_plain = ecdsa(key, inner)
    if attempt("Candidate 2 — plain ECDSA over ClobAuth digest (diagnostic)",
               WALLET, sig_plain, ts, args.nonce):
        return 0

    print("\nNo candidate registered a key to the deposit wallet. "
          "Read the server messages above — they tell us what it wants next.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
