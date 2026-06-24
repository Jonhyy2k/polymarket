#pragma once

// EIP-712 typed-data hashing for Polymarket CLOB v2 orders (keyless, digest-only).
//
// ┌─ ⚠️  UNVERIFIED-AGAINST-CHAIN ────────────────────────────────────────────┐
// │ The struct layout and DOMAIN constants below were compiled from Polymarket │
// │ v2 docs + community references (captured 2026-06; see README "v2 signing   │
// │ spec"). They are NOT yet checked against the live on-chain contract. This  │
// │ module computes REAL Keccak-256 EIP-712 digests so the order path can be   │
// │ exercised without keys, but DO NOT trust a digest for a live signature     │
// │ until the printed ORDER_TYPEHASH / domain separator are reconciled with    │
// │ the deployed Exchange (see Eip712::self_describe()).                        │
// │ There is NO private key, NO signing, and NO network here — digest only.    │
// └────────────────────────────────────────────────────────────────────────────┘
//
// EIP-712: https://eips.ethereum.org/EIPS/eip-712
// Keccak-256 (Ethereum padding) via OpenSSL EVP "KECCAK-256" — verified to return
//   c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470 for "".

#include <array>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <string_view>

#include <openssl/evp.h>

namespace eip712 {

using Bytes32 = std::array<uint8_t, 32>;

// ── Keccak-256 (Ethereum variant) via OpenSSL ───────────────────────────────
// The EVP_MD handle is immutable once fetched and safe to share across threads;
// fetched once for the process (intentionally leaked, freed by process exit).
// EVP_MD_CTX is per-call. This runs off the hot path (per order create/cancel).
inline const EVP_MD* keccak_md() noexcept {
    static EVP_MD* md = EVP_MD_fetch(nullptr, "KECCAK-256", nullptr);
    return md;
}

// Reused per-thread digest context — EVP_DigestInit_ex re-initializes it each
// call, so we avoid an EVP_MD_CTX_new/free (malloc/free) on every keccak. Freed
// on thread exit. Off the hot path, but a digest is several keccaks, so reuse
// keeps the sender-thread hand-off tight.
inline EVP_MD_CTX* keccak_ctx() noexcept {
    struct CtxHolder {
        EVP_MD_CTX* ctx = EVP_MD_CTX_new();
        ~CtxHolder() { if (ctx) EVP_MD_CTX_free(ctx); }
    };
    static thread_local CtxHolder holder;
    return holder.ctx;
}

inline Bytes32 keccak256(const uint8_t* data, size_t len) noexcept {
    Bytes32 out{};
    const EVP_MD* md = keccak_md();
    EVP_MD_CTX* ctx = keccak_ctx();
    if (!md || !ctx) return out;  // self_test() catches a missing provider loudly
    unsigned int n = 0;
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1 ||
        EVP_DigestUpdate(ctx, data, len) != 1 ||
        EVP_DigestFinal_ex(ctx, out.data(), &n) != 1) {
        out.fill(0);
    }
    return out;
}

inline Bytes32 keccak256(std::string_view s) noexcept {
    return keccak256(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ── ABI word encoders (all produce a 32-byte big-endian word) ───────────────

// uint8/uint64 → right-aligned big-endian 32-byte word.
inline Bytes32 word_u64(uint64_t v) noexcept {
    Bytes32 w{};
    for (int i = 0; i < 8; ++i) w[31 - i] = static_cast<uint8_t>(v >> (8 * i));
    return w;
}

// Decimal string (arbitrary precision, e.g. an ERC-1155 tokenId / uint256) →
// big-endian 32-byte word. Schoolbook w = w*10 + digit; bits above 256 are
// dropped (a valid tokenId fits in 256). Non-digits are skipped.
inline Bytes32 word_dec(std::string_view dec) noexcept {
    Bytes32 w{};
    for (char c : dec) {
        if (c < '0' || c > '9') continue;
        uint32_t carry = static_cast<uint32_t>(c - '0');
        for (int b = 31; b >= 0; --b) {
            const uint32_t v = static_cast<uint32_t>(w[b]) * 10u + carry;
            w[b] = static_cast<uint8_t>(v & 0xffu);
            carry = v >> 8;
        }
    }
    return w;
}

// 0x-hex string → right-aligned big-endian word (address: 20B → left-padded to
// 32; bytes32: 32B exact). Parses pairs from the right; invalid chars stop it.
inline Bytes32 word_hex(std::string_view hex) noexcept {
    Bytes32 w{};
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex.remove_prefix(2);
    }
    auto nyb = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int i = static_cast<int>(hex.size()) - 1;
    int b = 31;
    while (i >= 0 && b >= 0) {
        const int lo = nyb(hex[i]);
        const int hi = (i - 1 >= 0) ? nyb(hex[i - 1]) : 0;
        if (lo < 0 || hi < 0) break;
        w[b] = static_cast<uint8_t>((hi << 4) | lo);
        --b;
        i -= 2;
    }
    return w;
}

inline std::string to_hex(const Bytes32& w) {
    static const char* k = "0123456789abcdef";
    std::string s = "0x";
    s.reserve(66);
    for (uint8_t byte : w) { s.push_back(k[byte >> 4]); s.push_back(k[byte & 0xf]); }
    return s;
}

// ── v2 Order type + domain ──────────────────────────────────────────────────
// encodeType MUST match the deployed struct exactly (field order, types, names,
// no spaces). ORDER_TYPEHASH = keccak256(this string).
inline constexpr std::string_view kOrderTypeString =
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)";

inline constexpr std::string_view kDomainTypeString =
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";

struct Domain {
    std::string name = "Polymarket CTF Exchange";
    std::string version = "2";
    uint64_t    chain_id = 137;                 // Polygon mainnet
    std::string verifying_contract = "0xE111180000d2663C0091e4f400237545B87B996B";
};

// Canonical v2 domains (see header banner — UNVERIFIED vs on-chain).
namespace polymarket_v2 {
    // Standard CTF Exchange.
    inline Domain standard() {
        return Domain{"Polymarket CTF Exchange", "2", 137,
                      "0xE111180000d2663C0091e4f400237545B87B996B"};
    }
    // Neg-risk CTF Exchange. NOTE: the domain `name` is the one unresolved field
    // — the official migration doc shows "Polymarket CTF Exchange" for both, but
    // a community cheatsheet shows "Polymarket Neg Risk CTF Exchange". We default
    // to the migration-doc value; flip kNegRiskName if chain verification says so.
    inline constexpr std::string_view kNegRiskName = "Polymarket CTF Exchange";
    inline Domain neg_risk() {
        return Domain{std::string(kNegRiskName), "2", 137,
                      "0xe2222d279d744050d28e00520010520000310F59"};
    }
}  // namespace polymarket_v2

inline const Bytes32& order_typehash() noexcept {
    static const Bytes32 h = keccak256(kOrderTypeString);
    return h;
}

inline Bytes32 domain_separator(const Domain& d) {
    static const Bytes32 domain_typehash = keccak256(kDomainTypeString);
    uint8_t buf[32 * 5];
    std::memcpy(buf + 0,   domain_typehash.data(), 32);
    const Bytes32 nh = keccak256(d.name);              std::memcpy(buf + 32,  nh.data(), 32);
    const Bytes32 vh = keccak256(d.version);           std::memcpy(buf + 64,  vh.data(), 32);
    const Bytes32 ci = word_u64(d.chain_id);           std::memcpy(buf + 96,  ci.data(), 32);
    const Bytes32 vc = word_hex(d.verifying_contract); std::memcpy(buf + 128, vc.data(), 32);
    return keccak256(buf, sizeof(buf));
}

namespace polymarket_v2 {
    // The two v2 domains are constant, so their separators are computed once and
    // cached. Per-order digests should use these instead of recomputing
    // keccak(name)/keccak(version)/keccak(domain) every time (~3 keccaks saved).
    inline const Bytes32& standard_separator() {
        static const Bytes32 s = domain_separator(standard());
        return s;
    }
    inline const Bytes32& neg_risk_separator() {
        static const Bytes32 s = domain_separator(neg_risk());
        return s;
    }
}  // namespace polymarket_v2

// Fully-encoded order fields (already in ABI-word form), in struct order.
struct OrderWords {
    Bytes32 salt;
    Bytes32 maker;
    Bytes32 signer;
    Bytes32 token_id;
    Bytes32 maker_amount;
    Bytes32 taker_amount;
    Bytes32 side;
    Bytes32 signature_type;
    Bytes32 timestamp;
    Bytes32 metadata;
    Bytes32 builder;
};

inline Bytes32 hash_struct(const OrderWords& o) {
    uint8_t buf[32 * 12];
    std::memcpy(buf + 0,   order_typehash().data(), 32);
    std::memcpy(buf + 32,  o.salt.data(),           32);
    std::memcpy(buf + 64,  o.maker.data(),          32);
    std::memcpy(buf + 96,  o.signer.data(),         32);
    std::memcpy(buf + 128, o.token_id.data(),       32);
    std::memcpy(buf + 160, o.maker_amount.data(),   32);
    std::memcpy(buf + 192, o.taker_amount.data(),   32);
    std::memcpy(buf + 224, o.side.data(),           32);
    std::memcpy(buf + 256, o.signature_type.data(), 32);
    std::memcpy(buf + 288, o.timestamp.data(),      32);
    std::memcpy(buf + 320, o.metadata.data(),       32);
    std::memcpy(buf + 352, o.builder.data(),        32);
    return keccak256(buf, sizeof(buf));
}

// EIP-712 digest = keccak256(0x1901 ‖ domainSeparator ‖ hashStruct(order)).
// This is the 32-byte payload an EOA would ECDSA-sign. We stop here (no key).
inline Bytes32 typed_data_hash(const Bytes32& domain_sep, const Bytes32& struct_hash) {
    uint8_t buf[2 + 32 + 32];
    buf[0] = 0x19;
    buf[1] = 0x01;
    std::memcpy(buf + 2,  domain_sep.data(),  32);
    std::memcpy(buf + 34, struct_hash.data(), 32);
    return keccak256(buf, sizeof(buf));
}

// One-time sanity that the Keccak provider is present and Ethereum-correct.
// Returns true iff keccak256("") matches the canonical empty-string vector.
inline bool self_test() noexcept {
    static const Bytes32 expect = {
        0xc5,0xd2,0x46,0x01,0x86,0xf7,0x23,0x3c,0x92,0x7e,0x7d,0xb2,0xdc,0xc7,0x03,0xc0,
        0xe5,0x00,0xb6,0x53,0xca,0x82,0x27,0x3b,0x7b,0xfa,0xd8,0x04,0x5d,0x85,0xa4,0x70};
    return keccak256(std::string_view{}) == expect;
}

}  // namespace eip712
