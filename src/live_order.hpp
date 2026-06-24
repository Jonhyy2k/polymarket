#pragma once

// v2 LiveOrderPayload + mapping (pure, keyless). Turns a resting reward quote
// (token, side, price-thou, size-shares) into the concrete Polymarket CLOB v2
// order fields a signer would need, then into the EIP-712 digest via eip712.hpp.
//
// NO private key, NO signature, NO network. The digest produced here is the
// 32-byte payload an EOA *would* sign — we stop one step short of signing so the
// whole serialization/hash path is exercised and measurable without custody.
//
// Amount convention (Polymarket CLOB): collateral (pUSD) and outcome tokens are
// both 6-decimal base units. For price p (thousandths) and size q (whole shares):
//   BUY : makerAmount = q·p·1000  (pUSD you provide),  takerAmount = q·1e6 (shares)
//   SELL: makerAmount = q·1e6      (shares you provide), takerAmount = q·p·1000 (pUSD)
// (1e6/1000 = 1000, so q·p·1000 is exact integer USDC base.)  ── verify vs spec.

#include "eip712.hpp"
#include "rewards.hpp"
#include "types.hpp"

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>

namespace live {

// signatureType enum (Polymarket): 0 EOA, 1 POLY_PROXY, 2 POLY_GNOSIS_SAFE.
enum class SigType : uint8_t { EOA = 0, POLY_PROXY = 1, POLY_GNOSIS_SAFE = 2 };

// Static signer identity + routing for the (mock) live path. Addresses are
// placeholders until real custody exists; they do not gate the digest math.
struct SignerConfig {
    std::string maker  = "0x0000000000000000000000000000000000000000";  // funder/proxy or EOA
    std::string signer = "0x0000000000000000000000000000000000000000";  // key that signs
    SigType     signature_type = SigType::EOA;
};

// One v2 order in human-readable form (pre-encoding).
struct LiveOrderPayload {
    uint64_t    salt = 0;
    std::string maker;
    std::string signer;
    std::string token_id;      // ERC-1155 position id, decimal uint256 string
    uint64_t    maker_amount = 0;
    uint64_t    taker_amount = 0;
    uint8_t     side = 0;          // 0 BUY / 1 SELL
    uint8_t     signature_type = 0;
    uint64_t    timestamp_ms = 0;  // ms — uniqueness, not expiry (v2)
    bool        neg_risk = false;  // selects verifying contract (+ maybe domain name)
};

// pUSD/share base-unit amounts for a (side, price-thou, size-shares) quote.
inline void amounts_for(bool is_buy, Price price_thou, Size size,
                        uint64_t& maker_amount, uint64_t& taker_amount) noexcept {
    const uint64_t shares_base = static_cast<uint64_t>(size) * 1000000ull;
    const uint64_t usdc_base   = static_cast<uint64_t>(size) *
                                 static_cast<uint64_t>(price_thou) * 1000ull;
    if (is_buy) { maker_amount = usdc_base;   taker_amount = shares_base; }
    else        { maker_amount = shares_base; taker_amount = usdc_base; }
}

inline LiveOrderPayload make_payload(const SignerConfig& cfg, std::string_view token_id,
                                     bool is_buy, Price price_thou, Size size,
                                     uint64_t salt, uint64_t timestamp_ms,
                                     bool neg_risk) {
    LiveOrderPayload p;
    p.salt = salt;
    p.maker = cfg.maker;
    p.signer = cfg.signer;
    p.token_id.assign(token_id.begin(), token_id.end());
    amounts_for(is_buy, price_thou, size, p.maker_amount, p.taker_amount);
    p.side = is_buy ? 0 : 1;
    p.signature_type = static_cast<uint8_t>(cfg.signature_type);
    p.timestamp_ms = timestamp_ms;
    p.neg_risk = neg_risk;
    return p;
}

inline eip712::OrderWords encode(const LiveOrderPayload& p) {
    eip712::OrderWords w;
    w.salt           = eip712::word_u64(p.salt);
    w.maker          = eip712::word_hex(p.maker);
    w.signer         = eip712::word_hex(p.signer);
    w.token_id       = eip712::word_dec(p.token_id);
    w.maker_amount   = eip712::word_u64(p.maker_amount);
    w.taker_amount   = eip712::word_u64(p.taker_amount);
    w.side           = eip712::word_u64(p.side);
    w.signature_type = eip712::word_u64(p.signature_type);
    w.timestamp      = eip712::word_u64(p.timestamp_ms);
    // metadata / builder are zero (no builder code attached).
    return w;
}

// Full EIP-712 digest for this order under the correct v2 domain. Uses the cached
// domain separator (constant), so a digest costs just 2 keccaks: hash_struct +
// the 0x1901 wrap.
inline eip712::Bytes32 digest(const LiveOrderPayload& p) {
    const eip712::Bytes32& dom_sep =
        p.neg_risk ? eip712::polymarket_v2::neg_risk_separator()
                   : eip712::polymarket_v2::standard_separator();
    return eip712::typed_data_hash(dom_sep, eip712::hash_struct(encode(p)));
}

// ── Pre-send quote validation (feeds near_miss_live.log) ────────────────────
// Catches quotes that a live venue would reject or that would not score rewards:
//   off-grid    — price not a multiple of the market tick
//   too_wide    — a side sits beyond max_spread of mid (earns no reward)
//   crossed     — bid >= ask (self-cross)
struct QuoteIssues {
    bool off_grid = false;
    bool too_wide = false;
    bool crossed  = false;
    bool any() const noexcept { return off_grid || too_wide || crossed; }
};

inline QuoteIssues validate_quote(const DesiredQuotes& q, Price tick,
                                  Price max_spread_thou, uint32_t mid2) noexcept {
    QuoteIssues s;
    if (tick == 0) return s;  // unknown tick: cannot judge grid
    auto off = [&](Price px) { return (px % tick) != 0; };
    auto wide = [&](Price px) {
        if (mid2 == 0 || max_spread_thou == 0) return false;
        const long s2 = std::labs(static_cast<long>(px) * 2 - static_cast<long>(mid2));
        return s2 >= static_cast<long>(max_spread_thou) * 2;
    };
    if (q.bid.valid) { s.off_grid |= off(q.bid.price); s.too_wide |= wide(q.bid.price); }
    if (q.ask.valid) { s.off_grid |= off(q.ask.price); s.too_wide |= wide(q.ask.price); }
    if (q.bid.valid && q.ask.valid && q.bid.price >= q.ask.price) s.crossed = true;
    return s;
}

}  // namespace live
