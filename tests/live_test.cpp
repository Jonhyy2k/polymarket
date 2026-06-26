// Deterministic checks for the (mock) live path: Keccak-256 / EIP-712, the v2
// order mapping, the MockLiveGateway, the adaptive throttle, and the pUSD
// allowance gate. NO keys, NO signing, NO network. Mirrors test_executor's style.

#include "eip712.hpp"
#include "live_order.hpp"
#include "mock_live_gateway.hpp"
#include "throttle.hpp"
#include "rewards.hpp"
#include "acr.hpp"
#include "oms.hpp"
#include "clob_auth.hpp"
#include "fill_parser.hpp"
#include "eip712_sign.hpp"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
                              else { std::printf("ok:   %s\n", msg); } } while (0)

static Orderbook make_book(Price bid, Price ask, Price tick) {
    Orderbook b{};
    b.best_bid = bid; b.best_ask = ask; b.tick_thou = tick;
    b.best_bid_size = 1000; b.best_ask_size = 1000;
    return b;
}

int main() {
    using namespace eip712;

    // ---- Keccak-256 is Ethereum's (not NIST SHA3) ----
    {
        CHECK(self_test(), "keccak256(\"\") matches canonical Ethereum vector");
        const Bytes32 abc = keccak256(std::string_view("abc"));
        CHECK(to_hex(abc) ==
              "0x4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45",
              "keccak256(\"abc\") matches known vector");
    }

    // ---- EIP-712 typehash + domain separators are stable (regression guard) ----
    // If the v2 Order struct string is ever mistyped, these change and the test
    // trips — exactly what we want before any of these feed a live signature.
    {
        CHECK(to_hex(order_typehash()) ==
              "0xbb86318a2138f5fa8ae32fbe8e659f8fcf13cc6ae4014a707893055433818589",
              "ORDER_TYPEHASH stable for the v2 struct");
        const Bytes32 sep_std = domain_separator(polymarket_v2::standard());
        const Bytes32 sep_neg = domain_separator(polymarket_v2::neg_risk());
        CHECK(sep_std != sep_neg, "standard vs neg-risk domains differ (different contract)");
        CHECK(to_hex(sep_std) ==
              "0x3264e159346253e26a64e00b69032db0e7d32f94628de3e6eecb50304d7af3d2",
              "standard domain separator stable");
    }

    // ---- word encoders ----
    {
        // uint64 -> right-aligned big-endian
        const Bytes32 w = word_u64(1);
        CHECK(w[31] == 1 && w[0] == 0, "word_u64 is big-endian right-aligned");
        // decimal uint256 round-trips a small value
        const Bytes32 d = word_dec("258");  // 258 = 0x0102
        CHECK(d[31] == 0x02 && d[30] == 0x01, "word_dec parses big decimal to BE bytes");
        // address: low 20 bytes set, high 12 zero
        const Bytes32 a = word_hex("0x000000000000000000000000000000000000beef");
        CHECK(a[31] == 0xef && a[30] == 0xbe && a[11] == 0x00, "word_hex left-pads address");
    }

    // ---- v2 amount mapping (pUSD/share 6dp) ----
    {
        uint64_t mk = 0, tk = 0;
        live::amounts_for(/*buy*/true, 480, 100, mk, tk);
        CHECK(mk == 48000000ull && tk == 100000000ull, "BUY: makerAmount=USDC, takerAmount=shares");
        live::amounts_for(/*buy*/false, 480, 100, mk, tk);
        CHECK(mk == 100000000ull && tk == 48000000ull, "SELL: makerAmount=shares, takerAmount=USDC");
    }

    // ---- digest determinism + sensitivity ----
    {
        live::SignerConfig cfg;
        const std::string tok = "71321045679252212594626385532706912750332728571942532289631379312455583992563";
        auto p  = live::make_payload(cfg, tok, true, 480, 100, 42, 1700000000000ull, false);
        auto p2 = live::make_payload(cfg, tok, true, 480, 100, 42, 1700000000000ull, false);
        CHECK(live::digest(p) == live::digest(p2), "identical payload -> identical digest");
        auto p3 = p2; // change one field
        auto pp3 = live::make_payload(cfg, tok, true, 481, 100, 42, 1700000000000ull, false);
        CHECK(live::digest(p) != live::digest(pp3), "price change -> different digest");
        auto pneg = live::make_payload(cfg, tok, true, 480, 100, 42, 1700000000000ull, true);
        CHECK(live::digest(p) != live::digest(pneg), "neg-risk domain -> different digest");
    }

    // ---- MockLiveGateway: one EIP-712 digest per create, mode-agnostic OMS ----
    {
        live::MockLiveGateway gw(live::SignerConfig{}, false);
        RiskLimits limits; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
        Oms oms(gw, limits);
        const std::string tok = "12345678901234567890";
        DesiredQuotes q{}; q.bid = {480, 100, true}; q.ask = {520, 100, true};
        oms.reconcile(tok, q, 1000, /*neg_risk*/false);
        CHECK(gw.stats().digests == 2 && oms.stats().creates == 2,
              "mocklive: digests == creates (one per CREATE)");
        q.bid.price = 481;  // reprice one side -> cancel+create
        oms.reconcile(tok, q, 1000, false);
        CHECK(gw.stats().digests == 3, "mocklive: reprice computes another digest");
        CHECK(gw.stats().cancels == 1, "mocklive: reprice cancels the old side");
    }

    // ---- OMS behavior is identical Shadow vs MockLive (mode-agnostic) ----
    {
        auto run = [](IExecGateway& gw) {
            RiskLimits limits; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
            Oms oms(gw, limits);
            DesiredQuotes q{}; q.bid = {490, 100, true}; q.ask = {510, 100, true};
            oms.reconcile("T", q);
            oms.reconcile("T", q);            // idempotent
            q.ask.price = 511; oms.reconcile("T", q);  // reprice ask
            DesiredQuotes none{}; oms.reconcile("T", none);  // cancel all
            return oms.stats();
        };
        ShadowGateway sg(false);
        live::MockLiveGateway mg(live::SignerConfig{}, false);
        const OmsStats s = run(sg);
        const OmsStats m = run(mg);
        CHECK(s.creates == m.creates && s.cancels == m.cancels && s.replaces == m.replaces,
              "OMS stats identical regardless of gateway (mode-agnostic)");
    }

    // ---- pUSD allowance gate ----
    {
        ShadowGateway gw(false);
        RiskLimits limits; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
        limits.max_collateral_usd = 60.0;  // approve only $60 of pUSD
        Oms oms(gw, limits);
        DesiredQuotes q{}; q.bid = {500, 100, true}; q.ask = {500, 100, true};  // $50 + $50 > $60
        oms.reconcile("T", q);
        CHECK(oms.stats().risk_rejects >= 1, "allowance: gross beyond approved pUSD is rejected");
        CHECK(oms.open_order_count() == 1, "allowance: first side fits, second rejected");
    }

    // ---- adaptive throttle ----
    {
        ThrottleConfig cfg; cfg.enabled = true; cfg.min_interval_ms = 10; cfg.max_interval_ms = 80;
        cfg.vol_hot_thou = 5.0f;
        Orderbook b = make_book(490, 510, 10);

        ThrottleConfig off; off.enabled = false;
        CHECK(QuoteThrottle::allow(b, off, 0, false), "throttle disabled -> always allow");

        const uint64_t MS = 1000000ull;
        // first calm quote at t=0 allowed (no prior), interval grows toward max
        CHECK(QuoteThrottle::allow(b, cfg, 0, false), "throttle: first quote allowed");
        // a re-price 1ms later (must_act=false) is held
        CHECK(!QuoteThrottle::allow(b, cfg, 1 * MS, false), "throttle: re-price within interval held");
        // ACR / side-change must bypass even within the interval
        CHECK(QuoteThrottle::allow(b, cfg, 1 * MS, /*must_act*/true), "throttle: must_act bypasses");
        // after enough time, allowed again
        CHECK(QuoteThrottle::allow(b, cfg, 200 * MS, false), "throttle: allowed after interval elapsed");

        // calm grows the interval; hot shrinks it
        Orderbook calm = make_book(490, 510, 10); calm.acr_vol_thou = 0.0f;
        QuoteThrottle::allow(calm, cfg, 0, false);
        const uint32_t iv1 = calm.thr_interval_ms;
        QuoteThrottle::allow(calm, cfg, 100 * MS, false);
        CHECK(calm.thr_interval_ms >= iv1, "throttle: calm market lengthens cadence");

        Orderbook hot = make_book(490, 510, 10); hot.acr_vol_thou = 50.0f;
        hot.thr_interval_ms = 80; hot.thr_last_quote_ns = 0;
        QuoteThrottle::allow(hot, cfg, 1000 * MS, false);
        CHECK(hot.thr_interval_ms < 80, "throttle: volatile market shortens cadence");
    }

    // ---- pre-send quote validation ----
    {
        // off-grid bid (483 not a multiple of tick 10), crossed (bid>=ask)
        DesiredQuotes bad{}; bad.bid = {483, 100, true}; bad.ask = {481, 100, true};
        auto i1 = live::validate_quote(bad, 10, 45, 1000);
        CHECK(i1.off_grid && i1.crossed, "validate: off-grid + crossed flagged");
        // too wide: bid 60 thou from mid 500, max_spread 45
        DesiredQuotes wide{}; wide.bid = {440, 100, true}; wide.ask = {560, 100, true};
        auto i2 = live::validate_quote(wide, 10, 45, 1000);
        CHECK(i2.too_wide, "validate: side beyond max_spread flagged too_wide");
        // clean quote: on grid, inside spread, not crossed
        DesiredQuotes ok{}; ok.bid = {480, 100, true}; ok.ask = {520, 100, true};
        auto i3 = live::validate_quote(ok, 10, 45, 1000);
        CHECK(!i3.any(), "validate: clean quote has no issues");
    }

    // ---- dead-man's-switch mechanism (runtime kill + flatten + resume) ----
    {
        ShadowGateway gw(false);
        RiskLimits limits; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
        Oms oms(gw, limits);
        DesiredQuotes q{}; q.bid = {490, 100, true}; q.ask = {510, 100, true};
        oms.reconcile("T", q);
        CHECK(oms.open_order_count() == 2, "dms: orders placed before trip");
        oms.set_kill_switch(true);             // feed stale -> halt
        oms.cancel_everything();               // ...and flatten
        CHECK(oms.open_order_count() == 0, "dms: cancel_everything flattens");
        oms.reconcile("T", q);                 // blocked while killed
        CHECK(oms.open_order_count() == 0 && oms.kill_switch(), "dms: kill_switch blocks new orders");
        oms.set_kill_switch(false);            // feed resumed
        oms.reconcile("T", q);
        CHECK(oms.open_order_count() == 2, "dms: re-enabled after the switch clears");
    }

    // ---- CLOB L2 auth: HMAC-SHA256 + base64url vs a Python reference vector ----
    {
        // base64url("0123456789abcdef0123456789abcdef")
        const std::string secret = "MDEyMzQ1Njc4OWFiY2RlZjAxMjM0NTY3ODlhYmNkZWY=";
        const std::string msg = clob::l2_message(1700000000, "POST", "/order", "{\"x\":1}");
        CHECK(msg == "1700000000POST/order{\"x\":1}", "l2_message concatenates ts+method+path+body");
        const std::string sig = clob::hmac_sha256_b64url(secret, msg);
        // reference computed with python hmac/base64.urlsafe_b64encode
        CHECK(sig == "2t4TPZwjv6fDsCqe3ug1x5NQWmGEc82naGXsZQIdsWE=",
              "L2 HMAC-SHA256 base64url matches reference vector");

        // base64url round-trips arbitrary bytes (incl. values that map to -/_)
        const unsigned char raw[3] = {0xfb, 0xff, 0xbf};  // -> "-_-/" family
        std::string enc = clob::detail::b64_encode(raw, 3, true);
        auto dec = clob::detail::b64_decode(enc);
        CHECK(dec.size() == 3 && dec[0] == 0xfb && dec[1] == 0xff && dec[2] == 0xbf,
              "base64url encode/decode round-trips");

        clob::L2Creds c{"key-1", secret, "pass-1", "0xabc"};
        auto hdrs = clob::build_l2_headers(c, 1700000000, "POST", "/order", "{\"x\":1}");
        bool has_sig = false, has_key = false, has_ts = false, has_addr = false, has_pass = false;
        for (auto& h : hdrs) {
            if (h.first == "POLY_SIGNATURE")  has_sig  = (h.second == sig);
            if (h.first == "POLY_API_KEY")    has_key  = (h.second == "key-1");
            if (h.first == "POLY_TIMESTAMP")  has_ts   = (h.second == "1700000000");
            if (h.first == "POLY_ADDRESS")    has_addr = (h.second == "0xabc");
            if (h.first == "POLY_PASSPHRASE") has_pass = (h.second == "pass-1");
        }
        CHECK(hdrs.size() == 5 && has_sig && has_key && has_ts && has_addr && has_pass,
              "build_l2_headers emits all five POLY_* headers correctly");
    }

    // ---- fill parser: user-channel message -> FillEvent (string + numeric amounts) ----
    {
        // size_matched as a STRING (Polymarket commonly stringifies amounts)
        const std::string m1 =
            "{\"event_type\":\"order\",\"asset_id\":\"123456789\","
            "\"id\":\"0xdeadbeef\",\"status\":\"MATCHED\",\"side\":\"BUY\","
            "\"size_matched\":\"150\"}";
        live::FillEvent e1 = live::parse_user_message(m1);
        CHECK(e1.valid && e1.event_type == "order", "fill: order update recognized");
        CHECK(e1.asset_id == "123456789" && e1.exchange_order_id == "0xdeadbeef",
              "fill: asset_id + exchange order id extracted");
        CHECK(e1.size_matched == 150 && e1.status == "MATCHED" && e1.side == "BUY",
              "fill: string size_matched parsed to 150");

        // size_matched as a NUMBER
        const std::string m2 =
            "{\"event_type\":\"trade\",\"asset_id\":\"987\",\"size_matched\":42}";
        live::FillEvent e2 = live::parse_user_message(m2);
        CHECK(e2.valid && e2.size_matched == 42, "fill: numeric size_matched parsed to 42");

        // unrelated message -> not a fill
        const std::string m3 = "{\"event_type\":\"book\",\"asset_id\":\"1\"}";
        CHECK(!live::parse_user_message(m3).valid, "fill: non-order/trade message ignored");

        // malformed JSON -> safe, invalid
        CHECK(!live::parse_user_message("{not json").valid, "fill: malformed json is safe");
    }

    // ---- secp256k1 EIP-712 signing: byte-for-byte vs eth_account (RFC6979) ----
    // Reference vectors generated with eth_account 0.13.7 (Account.unsafe_sign_hash).
    // Throwaway test keys only — never real custody.
    {
        using namespace eip712;
        const std::string k1 = "0x0000000000000000000000000000000000000000000000000000000000000001";
        const std::string k2 = "0x4f3edf983ac636a65a842ce7c78d9aa706d3b113bce9c46f30d7d21715b23b1d";
        const Bytes32 d_ones = word_hex("0x1111111111111111111111111111111111111111111111111111111111111111");
        const Bytes32 d_dead = word_hex("0xdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");

        // address derivation (key=1 is the canonical vector)
        CHECK(address_from_privkey_hex(k1) == "0x7e5f4552091a69125d5dfcb7b8c2659029395bdf",
              "sign: address(privkey=1) matches canonical vector");
        CHECK(address_from_privkey_hex(k2) == "0x90f8bf6a479f320ead074411a4b0e7944ea8c9c1",
              "sign: address(privkey=k2) matches eth_account");

        Sig65 s{};
        CHECK(sign_digest_hex(d_ones, k1, s) &&
              sig_to_hex(s) == "0xe7c93726a865578504442b1a6827f676e0ed74bdff2be3960d1e253bbcfc4462"
                               "6aa772b878bc912bdbb33a0014ec507c4b3896ea85aa914b74dee9b7ac3e56da1c",
              "sign: k1/digest(0x11..) == eth_account (v=28)");
        CHECK(sign_digest_hex(d_dead, k1, s) &&
              sig_to_hex(s) == "0xfc734c415fbca1ee1650c1e4abc3b3848db40632b605c51c66eb0d3e2bed4858"
                               "6e42c65f430cfb3e3923cc024db9355738b6f878c70b6af2d76cd2d6e0a7cddf1b",
              "sign: k1/digest(0xdead..) == eth_account (v=27)");
        CHECK(sign_digest_hex(d_ones, k2, s) &&
              sig_to_hex(s) == "0x039d27b1425e8070c058e36f7f395f5f856998d14337e6b83c08b8b18ecc83bd"
                               "10c1c048ada88ac1b3314836a532c0cda0319c2a3cc6fdf64b5e963bfc19df6a1b",
              "sign: k2/digest(0x11..) == eth_account (v=27)");
        CHECK(sign_digest_hex(d_dead, k2, s) &&
              sig_to_hex(s) == "0xcb2d3062e0b4d0fa947edc06f9c0bc8d872ff8b05a82ac609eae1136fe4418fa"
                               "2986653ea1d5cbb9596032b6d27c02db0c89653e56cdf91d24e08c35410ba64d1b",
              "sign: k2/digest(0xdead..) == eth_account (v=27)");

        // low-s canonical: s must be <= n/2 (EIP-2). Top byte of s (out[32]) < 0x80
        // is a necessary check; eth_account vectors above already encode low-s.
        CHECK((s[32] & 0x80) == 0, "sign: s is low-s (canonical, EIP-2)");

        // end-to-end: sign a REAL order's EIP-712 digest, recover address == signer
        live::SignerConfig cfg;
        cfg.maker = cfg.signer = address_from_privkey_hex(k1);
        const auto p = live::make_payload(cfg, "123", true, 500, 200, 1, 1700000000000ull, false);
        const Bytes32 od = live::digest(p);
        Sig65 os{};
        CHECK(sign_digest_hex(od, k1, os), "sign: real order digest signs without error");
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
