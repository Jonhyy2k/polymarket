#!/usr/bin/env python3
"""
CLOB V2 order reference vectors — ground truth for the C++ encoder cross-check.

Builds the V2 EIP-712 typed data EXACTLY as @polymarket/clob-client-v2 does
(struct + domain confirmed against the live repo + GET /version → {"version":2}),
signs it with eth_account (the library our secp256k1 signer is already proven
byte-for-byte against), and prints the digest, signature, and the POST /order
wire body. The C++ V2 path must reproduce these for FIXED inputs before it is
ever used with real funds.

No network, no real key — a well-known test key only.
"""
import json

from eth_account import Account
from eth_account.messages import encode_typed_data

# Well-known test key (hardhat #1). NOT a real wallet.
KEY = "0x4f3edf983ac636a65a842ce7c78d9aa706d3b113bce9c46f30d7d21715b23b1d"
ADDR = Account.from_key(KEY).address  # 0x90F8bf6A479f320ead074411a4B0e7944Ea8c9C1

ZERO32 = "0x" + "00" * 32

# Fixed V2 order (standard exchange, BUY, EOA).
SALT = 479249096354
TOKEN_ID = "71321045679252212594626385532706912750332728571942532289631379312455583992563"
MAKER_AMOUNT = "1000000"
TAKER_AMOUNT = "2000000"
SIDE_INT = 0          # 0 = BUY, 1 = SELL  (in the SIGNED struct)
SIDE_STR = "BUY"      # string in the wire body
SIG_TYPE = 0          # EOA
TIMESTAMP_MS = "1713398400000"

# Standard CTF Exchange V2 domain (version "2"); neg-risk only swaps the contract.
DOMAIN_STD = "0xE111180000d2663C0091e4f400237545B87B996B"
DOMAIN_NEG = "0xe2222d279d744050d28e00520010520000310F59"

ORDER_TYPES = {
    "EIP712Domain": [
        {"name": "name", "type": "string"},
        {"name": "version", "type": "string"},
        {"name": "chainId", "type": "uint256"},
        {"name": "verifyingContract", "type": "address"},
    ],
    "Order": [
        {"name": "salt", "type": "uint256"},
        {"name": "maker", "type": "address"},
        {"name": "signer", "type": "address"},
        {"name": "tokenId", "type": "uint256"},
        {"name": "makerAmount", "type": "uint256"},
        {"name": "takerAmount", "type": "uint256"},
        {"name": "side", "type": "uint8"},
        {"name": "signatureType", "type": "uint8"},
        {"name": "timestamp", "type": "uint256"},
        {"name": "metadata", "type": "bytes32"},
        {"name": "builder", "type": "bytes32"},
    ],
}


def vector(verifying_contract: str, label: str):
    full_message = {
        "primaryType": "Order",
        "domain": {
            "name": "Polymarket CTF Exchange",
            "version": "2",
            "chainId": 137,
            "verifyingContract": verifying_contract,
        },
        "types": ORDER_TYPES,
        "message": {
            "salt": SALT,
            "maker": ADDR,
            "signer": ADDR,
            "tokenId": int(TOKEN_ID),
            "makerAmount": int(MAKER_AMOUNT),
            "takerAmount": int(TAKER_AMOUNT),
            "side": SIDE_INT,
            "signatureType": SIG_TYPE,
            "timestamp": int(TIMESTAMP_MS),
            "metadata": bytes.fromhex(ZERO32[2:]),
            "builder": bytes.fromhex(ZERO32[2:]),
        },
    }
    signable = encode_typed_data(full_message=full_message)
    signed = Account.sign_message(signable, KEY)
    digest = signed.message_hash.hex()
    sig = signed.signature.hex()
    digest = digest if digest.startswith("0x") else "0x" + digest
    sig = sig if sig.startswith("0x") else "0x" + sig

    print(f"--- {label} (verifyingContract={verifying_contract}) ---")
    print(f"signer       : {ADDR}")
    print(f"digest       : {digest}")
    print(f"signature    : {sig}")

    body = {
        "deferExec": False,
        "postOnly": False,
        "order": {
            "salt": SALT,
            "maker": ADDR,
            "signer": ADDR,
            "tokenId": TOKEN_ID,
            "makerAmount": MAKER_AMOUNT,
            "takerAmount": TAKER_AMOUNT,
            "side": SIDE_STR,
            "signatureType": SIG_TYPE,
            "timestamp": TIMESTAMP_MS,
            "expiration": "0",
            "metadata": ZERO32,
            "builder": ZERO32,
            "signature": sig,
        },
        "owner": "<api-key>",
        "orderType": "GTC",
    }
    print("wire body    :", json.dumps(body, separators=(",", ":")))
    print()
    return digest, sig


if __name__ == "__main__":
    print(f"address = {ADDR}\n")
    vector(DOMAIN_STD, "STANDARD V2")
    vector(DOMAIN_NEG, "NEG-RISK V2")
