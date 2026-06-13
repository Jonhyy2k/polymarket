#pragma once

#include "types.hpp"
#include <array>

struct ArbCheckOutput {
    static constexpr size_t kMaxOpportunities = 128;
    static constexpr size_t kMaxEdgeSamples = 128;

    uint16_t checks_performed = 0;
    size_t opportunity_count = 0;
    size_t edge_sample_count = 0;
    std::array<ArbOpportunity, kMaxOpportunities> opportunities{};
    std::array<EdgeTelemetrySample, kMaxEdgeSamples> edge_samples{};
};

class ArbitrageDetector {
public:
    explicit ArbitrageDetector(const Config& config);

    // Check for arbitrage on a contract after any book update.
    ArbCheckOutput check(const Contract& contract, NanoTime t2_book_updated);
    ArbCheckOutput check_group(const ContractGroup& group, NanoTime t2_book_updated);

    // Stats
    uint64_t total_checks() const { return total_checks_; }
    uint64_t total_opportunities() const { return total_opps_; }
    uint64_t buy_both_count() const { return buy_both_; }
    uint64_t sell_both_count() const { return sell_both_; }
    uint64_t buy_group_count() const { return buy_group_; }
    uint64_t sell_group_count() const { return sell_group_; }
    uint64_t buy_no_sell_others_count() const { return buy_no_sell_others_; }
    uint64_t sell_no_buy_others_count() const { return sell_no_buy_others_; }
    uint64_t maker_buy_both_count() const { return maker_buy_both_; }
    uint64_t maker_sell_both_count() const { return maker_sell_both_; }
    uint64_t maker_buy_group_count() const { return maker_buy_group_; }
    uint64_t maker_sell_group_count() const { return maker_sell_group_; }
    uint64_t maker_buy_no_sell_others_count() const { return maker_buy_no_sell_others_; }
    uint64_t maker_sell_no_buy_others_count() const { return maker_sell_no_buy_others_; }

private:
    int per_share_fee_thou(const Contract& contract, Price price) const;
    const Config& config_;
    uint64_t total_checks_ = 0;
    uint64_t total_opps_   = 0;
    uint64_t buy_both_     = 0;
    uint64_t sell_both_    = 0;
    uint64_t buy_group_    = 0;
    uint64_t sell_group_   = 0;
    uint64_t buy_no_sell_others_ = 0;
    uint64_t sell_no_buy_others_ = 0;
    uint64_t maker_buy_both_ = 0;
    uint64_t maker_sell_both_ = 0;
    uint64_t maker_buy_group_ = 0;
    uint64_t maker_sell_group_ = 0;
    uint64_t maker_buy_no_sell_others_ = 0;
    uint64_t maker_sell_no_buy_others_ = 0;
};
