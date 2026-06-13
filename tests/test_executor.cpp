// Deterministic checks for the rewards strategy + shadow OMS. No network/keys.
#include "rewards.hpp"
#include "oms.hpp"

#include <cstdio>
#include <cmath>
#include <cassert>

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { std::printf("FAIL: %s\n", msg); ++g_failures; } \
                              else { std::printf("ok:   %s\n", msg); } } while (0)
static bool approx(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }

static Orderbook make_book(Price bid, Price ask, Price tick) {
    Orderbook b{};
    b.best_bid = bid; b.best_ask = ask; b.tick_thou = tick;
    b.best_bid_size = 1000; b.best_ask_size = 1000;
    return b;
}

int main() {
    // ---- scoring math (verified formula) ----
    // mid = 0.50 (500), max_spread = 45 thou (4.5c). Order at mid (s=0) => ((45-0)/45)^2 * size = size.
    {
        uint32_t mid2 = 1000; // mid 500
        double at_mid = spread_score(mid2, 500, 45, 100);
        CHECK(approx(at_mid, 100.0), "spread_score at mid = size");
        // order 20 thou from mid: ((45-20)/45)^2 * 100 = (25/45)^2*100 = 30.864...
        double off = spread_score(mid2, 480, 45, 100);
        CHECK(approx(off, std::pow(25.0/45.0, 2) * 100.0, 1e-6), "spread_score quadratic falloff");
        // at/beyond max_spread => 0
        CHECK(approx(spread_score(mid2, 455, 45, 100), 0.0), "spread_score zero at max_spread");
    }

    // ---- Qmin combine (c=3) ----
    {
        // central mid: max(min(q1,q2), max(q1/3,q2/3))
        CHECK(approx(combine_qmin(90, 30, 500), std::max(30.0, std::max(30.0, 10.0))), "Qmin central balanced");
        // one-sided central: q2=0 -> max(0, max(30,0)) = 30  (single side scores at 1/3 of 90)
        CHECK(approx(combine_qmin(90, 0, 500), 30.0), "Qmin central single-sided = q1/3");
        // extreme mid (<0.10): must be two-sided -> min
        CHECK(approx(combine_qmin(90, 0, 50), 0.0), "Qmin extreme single-sided = 0");
        CHECK(approx(combine_qmin(90, 30, 50), 30.0), "Qmin extreme two-sided = min");
    }

    // ---- quoting: two-sided, on-grid, within max_spread ----
    {
        RewardConfig cfg{}; cfg.active = true; cfg.max_spread_thou = 45; cfg.min_size = 20; cfg.daily_rate_usd = 250;
        RewardQuoteParams p{}; p.quote_size = 100; p.target_offset_thou = 0; // tightest
        Orderbook book = make_book(490, 510, 10); // mid 500, tick 0.01
        DesiredQuotes q = RewardQuoter::quote(book, cfg, p);
        CHECK(q.bid.valid && q.ask.valid, "quote: two-sided produced");
        CHECK(q.bid.price % 10 == 0 && q.ask.price % 10 == 0, "quote: on tick grid");
        CHECK(q.bid.price < q.ask.price, "quote: bid below ask");
        CHECK(q.bid.price <= 500 && q.ask.price >= 500, "quote: straddles mid");
        CHECK(q.bid.price > book.best_bid - 50 && q.ask.price < book.best_ask + 50, "quote: near mid");
        CHECK(q.eligible && q.est_qmin > 0.0, "quote: eligible with positive Qmin");
    }
    // sub-min size => no quote
    {
        RewardConfig cfg{}; cfg.active = true; cfg.max_spread_thou = 45; cfg.min_size = 20;
        RewardQuoteParams p{}; p.quote_size = 10; // below min_size
        DesiredQuotes q = RewardQuoter::quote(make_book(490, 510, 10), cfg, p);
        CHECK(!q.bid.valid && !q.ask.valid, "quote: sub-min size suppressed");
    }
    // inactive rewards => no quote
    {
        RewardConfig cfg{}; cfg.active = false; cfg.max_spread_thou = 45; cfg.min_size = 20;
        RewardQuoteParams p{}; p.quote_size = 100;
        DesiredQuotes q = RewardQuoter::quote(make_book(490, 510, 10), cfg, p);
        CHECK(!q.eligible, "quote: inactive market suppressed");
    }

    // ---- OMS reconcile: create, idempotent, replace, cancel ----
    {
        ShadowGateway gw(false);
        RiskLimits limits{}; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
        Oms oms(gw, limits);
        const std::string tok = "TOKEN_A";

        DesiredQuotes q{}; q.bid = {490, 100, true}; q.ask = {510, 100, true};
        oms.reconcile(tok, q);
        CHECK(oms.stats().creates == 2, "oms: first reconcile creates 2");
        CHECK(oms.open_order_count() == 2, "oms: 2 open orders");

        oms.reconcile(tok, q);  // identical
        CHECK(oms.stats().creates == 2 && oms.stats().cancels == 0, "oms: identical reconcile is no-op");

        DesiredQuotes q2 = q; q2.bid.price = 491;  // bid moved
        oms.reconcile(tok, q2);
        CHECK(oms.stats().replaces == 1 && oms.stats().creates == 3 && oms.stats().cancels == 1,
              "oms: price change => cancel+recreate one side");

        DesiredQuotes none{}; // both invalid
        oms.reconcile(tok, none);
        CHECK(oms.open_order_count() == 0, "oms: empty desired cancels all");
    }

    // ---- OMS risk gate ----
    {
        ShadowGateway gw(false);
        RiskLimits limits{}; limits.max_gross_notional_usd = 50.0;  // tiny cap
        Oms oms(gw, limits);
        DesiredQuotes q{}; q.bid = {500, 100, true}; q.ask = {500, 100, true}; // $50 + $50 > cap
        oms.reconcile("T", q);
        CHECK(oms.stats().risk_rejects >= 1, "oms: gross notional cap rejects");

        RiskLimits killed{}; killed.kill_switch = true;
        Oms oms2(gw, killed);
        DesiredQuotes q3{}; q3.bid = {490, 20, true}; q3.ask = {510, 20, true};
        oms2.reconcile("T", q3);
        CHECK(oms2.open_order_count() == 0, "oms: kill switch blocks all");
    }

    // ---- OMS fills => position + filled notional ----
    {
        ShadowGateway gw(false);
        RiskLimits limits{}; limits.max_gross_notional_usd = 1e9; limits.max_position_shares = 1e9;
        Oms oms(gw, limits);
        DesiredQuotes q{}; q.bid = {500, 100, true}; q.ask = {520, 100, true};
        oms.reconcile("T", q);
        oms.apply_fill("T", 1, 40);  // partial fill of the bid (client_id 1)
        CHECK(approx(oms.net_position("T"), 40.0), "oms: partial fill updates position");
        CHECK(oms.open_order_count() == 2, "oms: partially-filled order still open");
        oms.apply_fill("T", 1, 60);  // remainder
        CHECK(approx(oms.net_position("T"), 100.0), "oms: full fill");
        CHECK(oms.open_order_count() == 1, "oms: filled order removed");
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
