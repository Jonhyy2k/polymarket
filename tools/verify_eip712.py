#!/usr/bin/env python3
"""
Verify the v2 EIP-712 constants in src/eip712.hpp against the on-chain Polymarket
Exchange contracts — closes README break #3 ("UNVERIFIED on-chain").

It (1) independently recomputes ORDER_TYPEHASH + the standard/neg-risk domain
separators (catching any drift vs the C++/OpenSSL implementation), and (2)
eth_call's each Exchange's DOMAIN_SEPARATOR() on Polygon and compares — resolving
the open neg-risk domain `name` by trying both candidates.

Zero third-party deps: a pure-Python Keccak-256 (Ethereum padding) + urllib JSON-RPC.
  python3 tools/verify_eip712.py                 # uses https://polygon-rpc.com
  python3 tools/verify_eip712.py --rpc <url>     # your own Polygon RPC
"""
import argparse
import json
import struct
import sys
import urllib.request

# ----------------------------- Keccak-256 -----------------------------------
_RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
    0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
    0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_MASK = (1 << 64) - 1


def _rol(v, n):
    return ((v << n) | (v >> (64 - n))) & _MASK


def _keccak_f(lanes):
    for rnd in range(24):
        # θ
        C = [lanes[x][0] ^ lanes[x][1] ^ lanes[x][2] ^ lanes[x][3] ^ lanes[x][4] for x in range(5)]
        D = [C[(x - 1) % 5] ^ _rol(C[(x + 1) % 5], 1) for x in range(5)]
        for x in range(5):
            for y in range(5):
                lanes[x][y] ^= D[x]
        # ρ and π
        x, y = 1, 0
        cur = lanes[x][y]
        for t in range(24):
            x, y = y, (2 * x + 3 * y) % 5
            cur, lanes[x][y] = lanes[x][y], _rol(cur, ((t + 1) * (t + 2) // 2) % 64)
        # χ
        for y in range(5):
            T = [lanes[x][y] for x in range(5)]
            for x in range(5):
                lanes[x][y] = T[x] ^ ((~T[(x + 1) % 5]) & T[(x + 2) % 5])
        # ι
        lanes[0][0] ^= _RC[rnd]
    return lanes


def _permute(state):
    lanes = [[struct.unpack('<Q', state[8 * (x + 5 * y):8 * (x + 5 * y) + 8])[0]
              for y in range(5)] for x in range(5)]
    lanes = _keccak_f(lanes)
    out = bytearray(200)
    for x in range(5):
        for y in range(5):
            out[8 * (x + 5 * y):8 * (x + 5 * y) + 8] = struct.pack('<Q', lanes[x][y])
    return out


def keccak256(msg: bytes) -> bytes:
    rate = 136  # bytes (1088-bit rate for 256-bit output)
    state = bytearray(200)
    msg = bytearray(msg)
    off = 0
    while len(msg) - off >= rate:
        for i in range(rate):
            state[i] ^= msg[off + i]
        state = _permute(state)
        off += rate
    block = bytearray(msg[off:])
    block.append(0x01)               # Ethereum/Keccak pad10*1 (NOT 0x06 SHA3)
    while len(block) < rate:
        block.append(0x00)
    block[rate - 1] ^= 0x80
    for i in range(rate):
        state[i] ^= block[i]
    state = _permute(state)
    return bytes(state[:32])


# self-test — abort if the hash is wrong so we never "verify" with broken keccak
assert keccak256(b"").hex() == \
    "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470", "keccak256 broken"

# ----------------------------- EIP-712 --------------------------------------
ORDER_TYPE = (b"Order(uint256 salt,address maker,address signer,uint256 tokenId,"
              b"uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
              b"uint256 timestamp,bytes32 metadata,bytes32 builder)")
DOMAIN_TYPE = b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"

# Must match src/eip712.hpp / the live_test regression guard.
EXPECT_TYPEHASH = "0xbb86318a2138f5fa8ae32fbe8e659f8fcf13cc6ae4014a707893055433818589"


def _w_u256(v):
    return v.to_bytes(32, "big")


def _w_addr(addr):
    return bytes(12) + bytes.fromhex(addr[2:])


def domain_separator(name, version, chain_id, verifying):
    return keccak256(keccak256(DOMAIN_TYPE) + keccak256(name.encode()) +
                     keccak256(version.encode()) + _w_u256(chain_id) + _w_addr(verifying))


STD = ("Polymarket CTF Exchange", "2", 137, "0xE111180000d2663C0091e4f400237545B87B996B")
NEG_A = ("Polymarket CTF Exchange", "2", 137, "0xe2222d279d744050d28e00520010520000310F59")
NEG_B = ("Polymarket Neg Risk CTF Exchange", "2", 137, "0xe2222d279d744050d28e00520010520000310F59")

DOMAIN_SEPARATOR_SEL = "0x3644e515"  # keccak256("DOMAIN_SEPARATOR()")[:4]
EIP712DOMAIN_SEL = "0x84b0196e"      # eip712Domain() (EIP-5267) -> domain fields


def decode_eip712domain(hexdata):
    """Decode eip712Domain() return: (bytes1 fields, string name, string version,
    uint256 chainId, address verifyingContract, bytes32 salt, uint256[] extensions)."""
    raw = bytes.fromhex(hexdata[2:] if hexdata.startswith("0x") else hexdata)
    word = lambda i: raw[32 * i:32 * i + 32]
    u = lambda i: int.from_bytes(word(i), "big")

    def read_str(off):
        ln = int.from_bytes(raw[off:off + 32], "big")
        return raw[off + 32:off + 32 + ln].decode("utf-8", "replace")
    return {"name": read_str(u(1)), "version": read_str(u(2)), "chainId": u(3),
            "verifyingContract": "0x" + word(4)[12:].hex()}


CTF_SANITY = "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045"  # Conditional Tokens — MUST have code


def _rpc(rpc, method, params):
    req = {"jsonrpc": "2.0", "id": 1, "method": method, "params": params}
    r = urllib.request.urlopen(
        urllib.request.Request(rpc, json.dumps(req).encode(),
                               {"Content-Type": "application/json"}), timeout=20)
    return json.load(r).get("result")


def eth_call(rpc, to, data):
    return _rpc(rpc, "eth_call", [{"to": to, "data": data}, "latest"])


def eth_get_code(rpc, addr):
    return _rpc(rpc, "eth_getCode", [addr, "latest"])


def has_code(result):
    return bool(result) and result != "0x" and len(result) > 4


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rpc", default="https://polygon-rpc.com")
    args = ap.parse_args()

    print("== Offline: recompute our constants ==")
    th = "0x" + keccak256(ORDER_TYPE).hex()
    ok = (th == EXPECT_TYPEHASH)
    print(f"ORDER_TYPEHASH        = {th}  [{'matches eip712.hpp' if ok else 'DRIFT vs eip712.hpp!'}]")
    print(f"domain sep [standard] = 0x{domain_separator(*STD).hex()}")
    print(f"domain sep [neg A]    = 0x{domain_separator(*NEG_A).hex()}  (name='{NEG_A[0]}')")
    print(f"domain sep [neg B]    = 0x{domain_separator(*NEG_B).hex()}  (name='{NEG_B[0]}')")
    if not ok:
        print("!! Typehash drift — fix src/eip712.hpp before anything else."); return 1

    print(f"\n== On-chain via {args.rpc} ==")
    # Guard against stub/rate-limited RPCs that return empty for everything: a
    # known-good contract (CTF) MUST report code, or we'd false-negative below.
    try:
        if not has_code(eth_get_code(args.rpc, CTF_SANITY)):
            print(f"  ⚠️ RPC sanity FAILED — the CTF contract ({CTF_SANITY}) shows no code, so this\n"
                  f"     endpoint is returning stub/empty state (rate-limited or not Polygon mainnet).\n"
                  f"     Re-run with an authenticated RPC: --rpc <Alchemy/Infura/QuickNode/your node>")
            return 1
    except Exception as e:
        print(f"  RPC unreachable ({e}); re-run with --rpc <a working Polygon RPC>")
        return 1
    print("  RPC sanity OK (CTF has code).")

    checks = [("standard", STD[3], [STD]), ("neg-risk", NEG_A[3], [NEG_A, NEG_B])]
    any_fail = False
    for label, addr, cands in checks:
        print(f"\n[{label}] {addr}")
        if not has_code(eth_get_code(args.rpc, addr)):
            print("  ❌ no contract code at this address (RPC is healthy) — the address may be wrong; "
                  "check Polygonscan.")
            any_fail = True
            continue
        try:
            ds = eth_call(args.rpc, addr, DOMAIN_SEPARATOR_SEL)
        except Exception as e:
            print(f"  eth_call failed ({e}); re-run with --rpc <a working Polygon RPC>")
            any_fail = True
            continue
        # Preferred: DOMAIN_SEPARATOR() (one 32-byte word to compare directly).
        if ds and ds != "0x":
            ds = ds.lower()
            print(f"  on-chain DOMAIN_SEPARATOR = {ds}")
            match = next((c for c in cands if ("0x" + domain_separator(*c).hex()) == ds), None)
            if match:
                print(f"  ✅ VERIFIED — domain name = \"{match[0]}\"")
            else:
                any_fail = True
                print("  ❌ NO MATCH — our constants ≠ the deployed contract. DO NOT sign.")
            continue
        # Fallback: EIP-5267 eip712Domain() returns the domain fields directly,
        # which reads the on-chain `name`/`version` verbatim (resolves neg-risk name).
        try:
            dom = eth_call(args.rpc, addr, EIP712DOMAIN_SEL)
        except Exception as e:
            print(f"  eth_call(eip712Domain) failed ({e})")
            any_fail = True
            continue
        if not dom or dom == "0x":
            print("  exposes neither DOMAIN_SEPARATOR() nor eip712Domain() — verify manually")
            any_fail = True
            continue
        d = decode_eip712domain(dom)
        print(f"  on-chain eip712Domain: name=\"{d['name']}\" version=\"{d['version']}\" "
              f"chainId={d['chainId']} verifyingContract={d['verifyingContract']}")
        computed = "0x" + domain_separator(d["name"], d["version"], d["chainId"],
                                           d["verifyingContract"]).hex()
        match = next((c for c in cands if ("0x" + domain_separator(*c).hex()) == computed), None)
        if match:
            print(f"  ✅ VERIFIED — domain name = \"{match[0]}\" (separator {computed})")
        else:
            any_fail = True
            print(f"  ⚠️ on-chain name=\"{d['name']}\" → UPDATE eip712.hpp if it differs from "
                  f"our default \"{cands[0][0]}\" (on-chain separator {computed})")
    print("\n" + ("Some checks could not be confirmed — see above." if any_fail
                  else "All on-chain checks passed."))
    return 1 if any_fail else 0


if __name__ == "__main__":
    sys.exit(main())
