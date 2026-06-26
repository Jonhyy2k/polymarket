#pragma once

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ secp256k1 ECDSA signing over an EIP-712 digest — the custody primitive.     │
// │                                                                            │
// │ Produces the 65-byte Ethereum order signature r(32)|s(32)|v(1), v∈{27,28}, │
// │ canonical low-s, that Polymarket's CLOB expects. Uses libsecp256k1         │
// │ (RFC6979 deterministic nonce — same as eth_account/coincurve), so output   │
// │ is byte-for-byte reproducible and cross-checked against eth_account in      │
// │ live_test.                                                                  │
// │                                                                            │
// │ KEY HANDLING: the private key is passed in as bytes by the caller, which    │
// │ must load it from an operator-controlled source (env var / file) — it is    │
// │ NEVER generated, logged, or persisted here. Use a dedicated wallet funded    │
// │ with only the trading capital, not a primary wallet.                        │
// └────────────────────────────────────────────────────────────────────────────┘

#include "eip712.hpp"   // Bytes32, keccak256, word_hex, to_hex

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <string_view>

namespace eip712 {

using Sig65 = std::array<uint8_t, 65>;

namespace sign_detail {

// One process-wide context, randomized once (side-channel hardening). secp256k1
// contexts are safe to share across threads for sign/verify after creation.
inline secp256k1_context* ctx() {
    static secp256k1_context* c = [] {
        secp256k1_context* x = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        unsigned char seed[32];
        // Best-effort entropy for context randomization (not for key material).
        uint64_t t = static_cast<uint64_t>(std::time(nullptr)) ^
                     reinterpret_cast<uintptr_t>(&x);
        for (int i = 0; i < 32; ++i) seed[i] = static_cast<unsigned char>((t >> (i % 8 * 8)) ^ i);
        if (secp256k1_context_randomize(x, seed) != 1) {
            // Non-fatal: randomization is side-channel hardening only; signing is
            // still correct without it. Nothing to recover here.
        }
        return x;
    }();
    return c;
}

}  // namespace sign_detail

// 20-byte Ethereum address for a private key, lowercase "0x…40hex".
// address = keccak256(uncompressed_pubkey.x ‖ .y)[12:32]. Empty string on error.
inline std::string address_from_privkey(const Bytes32& priv) {
    secp256k1_pubkey pub;
    if (!secp256k1_ec_pubkey_create(sign_detail::ctx(), &pub, priv.data())) {
        return {};
    }
    unsigned char ser[65];
    size_t len = sizeof(ser);
    secp256k1_ec_pubkey_serialize(sign_detail::ctx(), ser, &len, &pub,
                                  SECP256K1_EC_UNCOMPRESSED);
    // ser = 0x04 ‖ X(32) ‖ Y(32); hash the 64 bytes after the prefix.
    const Bytes32 h = keccak256(ser + 1, 64);
    static const char* k = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(42);
    for (int i = 12; i < 32; ++i) { out.push_back(k[h[i] >> 4]); out.push_back(k[h[i] & 0xf]); }
    return out;
}

inline std::string address_from_privkey_hex(std::string_view priv_hex) {
    return address_from_privkey(word_hex(priv_hex));
}

// Sign a 32-byte EIP-712 digest. out = r(32)|s(32)|v(1), v∈{27,28}, low-s.
inline bool sign_digest(const Bytes32& digest, const Bytes32& priv, Sig65& out) {
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(sign_detail::ctx(), &sig, digest.data(),
                                          priv.data(), nullptr, nullptr)) {
        return false;
    }
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        sign_detail::ctx(), out.data(), &recid, &sig);   // fills out[0..63] = r‖s
    out[64] = static_cast<uint8_t>(recid + 27);           // Ethereum v
    return true;
}

inline bool sign_digest_hex(const Bytes32& digest, std::string_view priv_hex, Sig65& out) {
    return sign_digest(digest, word_hex(priv_hex), out);
}

// "0x" + 130 hex chars (65 bytes) — the order's signature field.
inline std::string sig_to_hex(const Sig65& s) {
    static const char* k = "0123456789abcdef";
    std::string out = "0x";
    out.reserve(132);
    for (uint8_t b : s) { out.push_back(k[b >> 4]); out.push_back(k[b & 0xf]); }
    return out;
}

}  // namespace eip712
