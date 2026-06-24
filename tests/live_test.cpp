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

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
