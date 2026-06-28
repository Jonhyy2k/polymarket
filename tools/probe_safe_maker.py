#!/usr/bin/env python3
"""
PROBE — does the CLOB accept a Gnosis-Safe (sigType 2) maker for our account?

Submits ONE post-only BUY far below the book (cannot cross) with:
    maker = SAFE (0xc78F…, undeployed + unfunded)   signer = EOA   signatureType = 2
authenticated with our existing EOA-bound L2 api key. Because the Safe holds no
funds and isn't deployed, the order CANNOT rest or fill — it is structurally
guaranteed to be rejected. We only read WHICH rejection:
    "not deployed" / "not enough balance / allowance"  -> maker TYPE accepted (GREEN)
    "maker address not allowed / use the deposit wallet flow" -> Safe also blocked (RED)

No funds move. Key/creds read from files; never printed.
"""
import base64, hashlib, hmac, json, os, secrets, time, urllib.request, urllib.error
from eth_account import Account
from eth_account.messages import encode_typed_data

CLOB = "https://clob.polymarket.com"
EXCHANGE_STD = "0xE111180000d2663C0091e4f400237545B87B996B"  # neg_risk=False
SAFE = "0xc78F48e6F1920029045293B527A1eE83F56a036F"          # derived sigType-2 Safe
TOKEN = "51331850329586411899922230603289784588200636687383537489773215611282099128373"
ZERO32 = "0x" + "00" * 32

ORDER_TYPES = {
    "EIP712Domain": [
        {"name": "name", "type": "string"}, {"name": "version", "type": "string"},
        {"name": "chainId", "type": "uint256"}, {"name": "verifyingContract", "type": "address"},
    ],
    "Order": [
        {"name": "salt", "type": "uint256"}, {"name": "maker", "type": "address"},
        {"name": "signer", "type": "address"}, {"name": "tokenId", "type": "uint256"},
        {"name": "makerAmount", "type": "uint256"}, {"name": "takerAmount", "type": "uint256"},
        {"name": "side", "type": "uint8"}, {"name": "signatureType", "type": "uint8"},
        {"name": "timestamp", "type": "uint256"}, {"name": "metadata", "type": "bytes32"},
        {"name": "builder", "type": "bytes32"},
    ],
}


def read_env(path):
    out = {}
    for line in open(path):
        line = line.strip()
        if line.startswith("export "):
            line = line[7:]
        if "=" in line and not line.startswith("#"):
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip().strip('"').strip("'")
    return out


def l2_headers(creds, address, method, path, body):
    ts = int(time.time())
    msg = f"{ts}{method}{path}{body}"
    sec = creds["secret"]
    sec += "=" * (-len(sec) % 4)
    mac = hmac.new(base64.urlsafe_b64decode(sec), msg.encode(), hashlib.sha256).digest()
    sig = base64.urlsafe_b64encode(mac).decode()
    return {
        "POLY_ADDRESS": address, "POLY_SIGNATURE": sig, "POLY_TIMESTAMP": str(ts),
        "POLY_API_KEY": creds["key"], "POLY_PASSPHRASE": creds["passphrase"],
        "User-Agent": "Mozilla/5.0", "Accept": "application/json",
        "Content-Type": "application/json",
    }


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--maker", default=SAFE)
    ap.add_argument("--sigtype", type=int, default=2)
    a = ap.parse_args()
    maker = a.maker
    sigtype = a.sigtype

    env = read_env("/home/ubuntu/.pm_creds.env")
    key = open("/home/ubuntu/.pm_signer_key").read().strip()
    eoa = Account.from_key(key).address
    creds = {"key": env["PM_API_KEY"], "secret": env["PM_API_SECRET"],
             "passphrase": env["PM_API_PASSPHRASE"]}

    salt = secrets.randbelow(2**63)
    ts_ms = str(int(time.time() * 1000))
    maker_amt, taker_amt = "100000", "5000000"      # BUY 5 shares @ 0.02 (= $0.10)
    signer_field = eoa if sigtype != 3 else maker     # POLY_1271 wants signer==maker
    msg = {
        "salt": salt, "maker": maker, "signer": signer_field, "tokenId": int(TOKEN),
        "makerAmount": int(maker_amt), "takerAmount": int(taker_amt),
        "side": 0, "signatureType": sigtype, "timestamp": int(ts_ms),
        "metadata": bytes(32), "builder": bytes(32),
    }
    full = {"primaryType": "Order",
            "domain": {"name": "Polymarket CTF Exchange", "version": "2",
                       "chainId": 137, "verifyingContract": EXCHANGE_STD},
            "types": ORDER_TYPES, "message": msg}
    signed = Account.sign_message(encode_typed_data(full_message=full), key)
    sig = signed.signature.hex()
    sig = sig if sig.startswith("0x") else "0x" + sig

    body_obj = {"deferExec": False, "postOnly": True,
                "order": {"salt": salt, "maker": maker, "signer": signer_field, "tokenId": TOKEN,
                          "makerAmount": maker_amt, "takerAmount": taker_amt, "side": "BUY",
                          "signatureType": sigtype, "timestamp": ts_ms, "expiration": "0",
                          "metadata": ZERO32, "builder": ZERO32, "signature": sig},
                "owner": creds["key"], "orderType": "GTC"}
    body = json.dumps(body_obj, separators=(",", ":"))

    print(f"signer      : {signer_field}")
    print(f"maker       : {maker}")
    print(f"order       : post-only BUY 5 @ 0.02 on standard exchange, sigType {sigtype}")
    headers = l2_headers(creds, eoa, "POST", "/order", body)
    req = urllib.request.Request(CLOB + "/order", data=body.encode(), method="POST", headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            status, resp = r.status, json.loads(r.read())
    except urllib.error.HTTPError as e:
        b = e.read().decode("utf-8", "ignore")
        try: b = json.loads(b)
        except Exception: pass
        status, resp = e.code, b
    print(f"\nPOST /order -> HTTP {status}")
    print("response:", json.dumps(resp) if isinstance(resp, (dict, list)) else resp)

    s = (json.dumps(resp) if isinstance(resp, (dict, list)) else str(resp)).lower()
    if "not allowed" in s or "deposit wallet" in s:
        print("\n🔴 RED: the CLOB rejects the Safe maker too — Safe path is closed.")
    elif any(w in s for w in ("balance", "allowance", "not deployed", "funds", "collateral")):
        print("\n🟢 GREEN: maker TYPE accepted; rejected only for funds/deploy — path is viable.")
    elif status == 200:
        print("\n⚠️ Unexpectedly ACCEPTED (200). It cannot rest (unfunded) but CANCEL to be safe.")
    else:
        print("\n❓ Inconclusive — read the message above to classify.")


if __name__ == "__main__":
    main()
