#pragma once

#include "types.hpp"

#include <chrono>
#include <array>
#include <fstream>
#include <limits>
#include <string>
#include <vector>
#include <algorithm>

class LatencyTracker {
public:
    static constexpr size_t MAX_SAMPLES = 16384;

    void record(double value_us) {
        samples_[count_ & (MAX_SAMPLES - 1)] = value_us;
        ++count_;
    }

    void reset() { count_ = 0; }

    struct Stats {
        double min = 0.0;
        double max = 0.0;
        double avg = 0.0;
        double p50 = 0.0;
        double p99 = 0.0;
        double p999 = 0.0;
        uint64_t count = 0;
    };

    Stats compute() const {
        Stats s{};
        const uint64_t n = std::min<uint64_t>(count_, MAX_SAMPLES);
        s.count = count_;
        if (n == 0) return s;

        std::vector<double> sorted(samples_, samples_ + n);
        std::sort(sorted.begin(), sorted.end());

        s.min = sorted.front();
        s.max = sorted.back();

        double sum = 0.0;
        for (double v : sorted) sum += v;
        s.avg = sum / n;
        s.p50 = sorted[static_cast<size_t>(n * 0.50)];
        s.p99 = sorted[static_cast<size_t>(n * 0.99)];
        s.p999 = sorted[std::min(static_cast<size_t>(n * 0.999), static_cast<size_t>(n - 1))];
        return s;
    }

private:
    double samples_[MAX_SAMPLES] = {};
    uint64_t count_ = 0;
};

class Logger {
public:
    explicit Logger(const Config& config);
    ~Logger();

    void consume_message(uint32_t bytes);
    void consume_book(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                      float arb_us, float e2e_us, uint16_t arb_checks);
    void consume_price_change(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                              float arb_us, float e2e_us, uint16_t arb_checks);
    void consume_bbo(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                     float arb_us, float e2e_us, uint16_t arb_checks);
    void consume_trade(uint16_t count);
    void consume_feed_delivery(float feed_delivery_ms);
    void consume_edge_sample(NanoTime sample_time_ns, std::string_view label, ArbKind kind,
                             uint16_t reference_value, uint8_t leg_count,
                             int raw_edge_bps, int net_edge_bps);
    void consume_replay_snapshot(const ReplaySnapshot& snapshot);
    void consume_opportunity(const ArbOpportunity& opp);
    void flush_pending_csv();

    void maybe_print_summary();
    void print_final_summary();

private:
    void note_live_event();
    bool benchmark_window_active();
    void print_summary(int interval_seconds);
    void reset_interval();

    struct EdgeTracker {
        void record(int raw_edge_bps, int net_edge_bps) {
            ++total;
            best_raw_bps = std::max(best_raw_bps, raw_edge_bps);
            best_net_bps = std::max(best_net_bps, net_edge_bps);
            ++raw_buckets[bucket_for(raw_edge_bps)];
        }

        static size_t bucket_for(int edge_bps) {
            if (edge_bps < 0) return 0;
            if (edge_bps < 5) return 1;
            if (edge_bps < 10) return 2;
            if (edge_bps < 20) return 3;
            return 4;
        }

        uint64_t total = 0;
        int best_raw_bps = std::numeric_limits<int>::min();
        int best_net_bps = std::numeric_limits<int>::min();
        std::array<uint64_t, 5> raw_buckets{};
    };

    int summary_interval_seconds_ = 60;
    int warmup_seconds_ = 0;
    bool metrics_enabled_ = true;
    bool edge_telemetry_enabled_ = true;
    bool flush_each_write_ = false;
    bool pending_csv_flush_ = false;
    bool warmup_completed_ = true;
    bool warmup_started_ = false;
    std::ofstream csv_file_;
    std::ofstream near_miss_file_;
    std::ofstream replay_file_;

    std::chrono::steady_clock::time_point run_start_;
    std::chrono::steady_clock::time_point warmup_start_;
    std::chrono::steady_clock::time_point warmup_end_;
    std::chrono::steady_clock::time_point interval_start_;

    uint64_t msg_count_ = 0;
    uint64_t book_count_ = 0;
    uint64_t price_change_count_ = 0;
    uint64_t bbo_count_ = 0;
    uint64_t trade_count_ = 0;
    uint64_t arb_checks_ = 0;
    uint64_t total_opps_ = 0;
    uint64_t buy_both_ = 0;
    uint64_t sell_both_ = 0;
    uint64_t buy_group_ = 0;
    uint64_t sell_group_ = 0;
    uint64_t buy_no_sell_others_ = 0;
    uint64_t sell_no_buy_others_ = 0;
    uint64_t maker_buy_both_ = 0;
    uint64_t maker_sell_both_ = 0;
    uint64_t maker_buy_group_ = 0;
    uint64_t maker_sell_group_ = 0;
    uint64_t maker_buy_no_sell_others_ = 0;
    uint64_t maker_sell_no_buy_others_ = 0;

    uint64_t interval_msgs_ = 0;
    uint64_t interval_books_ = 0;
    uint64_t interval_price_changes_ = 0;
    uint64_t interval_bbo_ = 0;
    uint64_t interval_trades_ = 0;
    uint64_t paper_trade_count_ = 0;

    double cumulative_paper_pnl_ = 0.0;
    double cumulative_paper_shares_ = 0.0;
    double cumulative_maker_theoretical_pnl_ = 0.0;

    LatencyTracker lat_queue_;
    LatencyTracker lat_parse_;
    LatencyTracker lat_book_;
    LatencyTracker lat_arb_;
    LatencyTracker lat_e2e_;
    LatencyTracker lat_feed_ms_;   // feed-delivery latency in MILLISECONDS (geography)
    LatencyTracker msg_sizes_;
    EdgeTracker edge_buy_both_;
    EdgeTracker edge_sell_both_;
    EdgeTracker edge_buy_group_;
    EdgeTracker edge_sell_group_;
    EdgeTracker edge_buy_no_sell_others_;
    EdgeTracker edge_sell_no_buy_others_;
    EdgeTracker edge_maker_buy_both_;
    EdgeTracker edge_maker_sell_both_;
    EdgeTracker edge_maker_buy_group_;
    EdgeTracker edge_maker_sell_group_;
    EdgeTracker edge_maker_buy_no_sell_others_;
    EdgeTracker edge_maker_sell_no_buy_others_;
};
