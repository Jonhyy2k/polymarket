#include "logger.hpp"

#include <cstdio>
#include <ctime>
#include <filesystem>

namespace {

constexpr const char* kOpportunityCsvHeader =
    "timestamp_ns,contract,arb_type,ask_yes,ask_no,bid_yes,bid_no,"
    "cost_or_proceeds,raw_edge_bps,edge_bps,size_shares,leg_count,theoretical_pnl,"
    "t0_recv_ns,t1_parse_ns,t2_book_ns,t3_arb_ns,t4_log_ns,"
    // recv_parse_us spans WS receipt -> parse done, so it includes queue wait +
    // parse + book apply (NOT pure parse time); named honestly to avoid misreads.
    "recv_parse_us,book_us,arb_us,e2e_us";

std::string utc_timestamp_suffix() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif

    char buffer[32];
    if (std::strftime(buffer, sizeof(buffer), "%Y%m%dT%H%M%SZ", &tm) == 0) {
        return "unknown";
    }
    return buffer;
}

void rotate_file_if_header_mismatch(const std::filesystem::path& path, std::string_view expected_header) {
    if (!std::filesystem::exists(path) || std::filesystem::is_empty(path)) {
        return;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return;
    }

    std::string first_line;
    std::getline(input, first_line);
    if (first_line == expected_header) {
        return;
    }

    std::filesystem::path rotated =
        path.parent_path() /
        (path.stem().string() + ".legacy-" + utc_timestamp_suffix() + path.extension().string());
    for (int suffix = 1; std::filesystem::exists(rotated); ++suffix) {
        rotated = path.parent_path() /
                  (path.stem().string() + ".legacy-" + utc_timestamp_suffix() + "-" +
                   std::to_string(suffix) + path.extension().string());
    }

    std::filesystem::rename(path, rotated);
    std::fprintf(stderr, "[LOGGER] Rotated legacy CSV header: %s -> %s\n",
                 path.string().c_str(), rotated.string().c_str());
}

}  // namespace

Logger::Logger(const Config& config)
    : summary_interval_seconds_(config.summary_interval_seconds)
    , warmup_seconds_(config.warmup_seconds)
    , metrics_enabled_(config.metrics_enabled)
    , edge_telemetry_enabled_(config.edge_telemetry_enabled)
    , flush_each_write_(config.flush_csv_each_write)
    , warmup_completed_(config.warmup_seconds <= 0) {
    run_start_ = std::chrono::steady_clock::now();
    warmup_start_ = run_start_;
    warmup_end_ = run_start_;
    interval_start_ = run_start_;

    auto dir = std::filesystem::path(config.log_file).parent_path();
    if (!dir.empty()) {
        std::filesystem::create_directories(dir);
    }
    auto near_miss_dir = std::filesystem::path(config.near_miss_log_file).parent_path();
    if (!near_miss_dir.empty()) {
        std::filesystem::create_directories(near_miss_dir);
    }
    auto replay_dir = std::filesystem::path(config.replay_log_file).parent_path();
    if (!replay_dir.empty()) {
        std::filesystem::create_directories(replay_dir);
    }

    rotate_file_if_header_mismatch(config.log_file, kOpportunityCsvHeader);

    if (warmup_seconds_ > 0) {
        std::printf("[LOGGER] Warmup enabled for %ds; timer starts on first live event\n",
                    warmup_seconds_);
        std::fflush(stdout);
    }

    csv_file_.open(config.log_file, std::ios::app);
    if (csv_file_.is_open()) {
        csv_file_.seekp(0, std::ios::end);
        if (csv_file_.tellp() == 0) {
            csv_file_ << kOpportunityCsvHeader << "\n";
            csv_file_.flush();
        }
    }

    if (edge_telemetry_enabled_ && !config.near_miss_log_file.empty()) {
        near_miss_file_.open(config.near_miss_log_file, std::ios::app);
        if (near_miss_file_.is_open()) {
            near_miss_file_.seekp(0, std::ios::end);
            if (near_miss_file_.tellp() == 0) {
                near_miss_file_ << "timestamp_ns,label,arb_type,reference_value,leg_count,raw_edge_bps,net_edge_bps\n";
                near_miss_file_.flush();
            }
        }
    }

    if (config.replay_logging_enabled && !config.replay_log_file.empty()) {
        replay_file_.open(config.replay_log_file, std::ios::app);
        if (replay_file_.is_open()) {
            replay_file_.seekp(0, std::ios::end);
            if (replay_file_.tellp() == 0) {
                replay_file_ << "frame_id,event_key,event_label,contract,leg_index,leg_count,touched_in_frame,"
                             << "frame_bytes,book_events,price_change_events,bbo_events,trade_events,"
                             << "t0_recv_ns,t1_parse_start_ns,t2_parse_done_ns,t3_books_done_ns,"
                             << "yes_exchange_ts,no_exchange_ts,"
                             << "yes_bid,yes_bid_size,yes_ask,yes_ask_size,"
                             << "no_bid,no_bid_size,no_ask,no_ask_size\n";
                replay_file_.flush();
            }
        }
    }
}

void Logger::note_live_event() {
    if (warmup_completed_ || warmup_started_ || warmup_seconds_ <= 0) {
        return;
    }

    warmup_started_ = true;
    warmup_start_ = std::chrono::steady_clock::now();
    warmup_end_ = warmup_start_ + std::chrono::seconds(warmup_seconds_);
    std::printf("[LOGGER] Warmup timer started on first live event\n");
    std::fflush(stdout);
}

bool Logger::benchmark_window_active() {
    if (warmup_completed_) {
        return true;
    }

    if (!warmup_started_) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < warmup_end_) {
        return false;
    }

    warmup_completed_ = true;
    reset_interval();
    interval_start_ = warmup_end_;
    std::printf("[LOGGER] Warmup complete after %ds of live feed; benchmark window started\n",
                warmup_seconds_);
    std::fflush(stdout);
    return true;
}

Logger::~Logger() {
    if (csv_file_.is_open()) {
        csv_file_.flush();
        csv_file_.close();
    }
    if (near_miss_file_.is_open()) {
        near_miss_file_.flush();
        near_miss_file_.close();
    }
    if (replay_file_.is_open()) {
        replay_file_.flush();
        replay_file_.close();
    }
}

void Logger::consume_message(uint32_t bytes) {
    note_live_event();
    if (!benchmark_window_active()) return;
    ++msg_count_;
    if (metrics_enabled_) {
        msg_sizes_.record(bytes);
    }
}

void Logger::consume_latency_event(uint64_t& counter, uint16_t count, bool has_latency_sample,
                                   float queue_us, float parse_us, float book_us,
                                   float arb_us, float e2e_us, uint16_t arb_checks) {
    note_live_event();
    if (!benchmark_window_active()) return;
    counter += count;
    if (!metrics_enabled_) return;
    if (has_latency_sample) {
        lat_queue_.record(queue_us);
        lat_parse_.record(parse_us);
        lat_book_.record(book_us);
    }
    if (has_latency_sample && arb_checks > 0) {
        lat_arb_.record(arb_us);
        lat_e2e_.record(e2e_us);
        arb_checks_ += arb_checks;
    }
}

void Logger::consume_book(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                          float arb_us, float e2e_us, uint16_t arb_checks) {
    consume_latency_event(book_count_, count, has_latency_sample, queue_us, parse_us, book_us,
                          arb_us, e2e_us, arb_checks);
}

void Logger::consume_price_change(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                                  float arb_us, float e2e_us, uint16_t arb_checks) {
    consume_latency_event(price_change_count_, count, has_latency_sample, queue_us, parse_us, book_us,
                          arb_us, e2e_us, arb_checks);
}

void Logger::consume_bbo(uint16_t count, bool has_latency_sample, float queue_us, float parse_us, float book_us,
                         float arb_us, float e2e_us, uint16_t arb_checks) {
    consume_latency_event(bbo_count_, count, has_latency_sample, queue_us, parse_us, book_us,
                          arb_us, e2e_us, arb_checks);
}

void Logger::consume_trade(uint16_t count) {
    note_live_event();
    if (!benchmark_window_active()) return;
    trade_count_ += count;
}

void Logger::consume_feed_delivery(float feed_delivery_ms) {
    note_live_event();
    if (!benchmark_window_active()) return;
    lat_feed_ms_.record(static_cast<double>(feed_delivery_ms));
}

void Logger::consume_edge_sample(NanoTime sample_time_ns, std::string_view label, ArbKind kind,
                                 uint16_t reference_value, uint8_t leg_count,
                                 int raw_edge_bps, int net_edge_bps) {
    note_live_event();
    if (!benchmark_window_active()) return;
    if (!edge_telemetry_enabled_) return;

    switch (kind) {
        case ArbKind::BUY_BOTH:
            edge_buy_both_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::SELL_BOTH:
            edge_sell_both_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::BUY_GROUP_YES:
            edge_buy_group_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::SELL_GROUP_YES:
            edge_sell_group_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::BUY_NO_SELL_OTHERS:
            edge_buy_no_sell_others_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::SELL_NO_BUY_OTHERS:
            edge_sell_no_buy_others_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_BUY_BOTH:
            edge_maker_buy_both_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_SELL_BOTH:
            edge_maker_sell_both_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_BUY_GROUP_YES:
            edge_maker_buy_group_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_SELL_GROUP_YES:
            edge_maker_sell_group_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
            edge_maker_buy_no_sell_others_.record(raw_edge_bps, net_edge_bps);
            break;
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
            edge_maker_sell_no_buy_others_.record(raw_edge_bps, net_edge_bps);
            break;
    }

    if (!near_miss_file_.is_open()) return;

    near_miss_file_ << sample_time_ns << ","
                    << label << ","
                    << arb_kind_name(kind) << ","
                    << reference_value << ","
                    << static_cast<unsigned>(leg_count) << ","
                    << raw_edge_bps << ","
                    << net_edge_bps << "\n";

    if (flush_each_write_) {
        pending_csv_flush_ = true;
    }
}

void Logger::consume_replay_snapshot(const ReplaySnapshot& snapshot) {
    note_live_event();
    if (!benchmark_window_active()) return;
    if (!replay_file_.is_open()) return;

    replay_file_ << snapshot.frame_id << ","
                 << snapshot.event_key << ","
                 << snapshot.event_label << ","
                 << snapshot.contract_name << ","
                 << static_cast<unsigned>(snapshot.leg_index) << ","
                 << static_cast<unsigned>(snapshot.leg_count) << ","
                 << (snapshot.touched_in_frame ? 1 : 0) << ","
                 << snapshot.frame_bytes << ","
                 << snapshot.book_events << ","
                 << snapshot.price_change_events << ","
                 << snapshot.bbo_events << ","
                 << snapshot.trade_events << ","
                 << snapshot.t0_ws_recv_ns << ","
                 << snapshot.t1_parse_start_ns << ","
                 << snapshot.t2_parse_done_ns << ","
                 << snapshot.t3_books_done_ns << ","
                 << snapshot.yes_exchange_ts << ","
                 << snapshot.no_exchange_ts << ","
                 << snapshot.yes_bid << ","
                 << snapshot.yes_bid_size << ","
                 << snapshot.yes_ask << ","
                 << snapshot.yes_ask_size << ","
                 << snapshot.no_bid << ","
                 << snapshot.no_bid_size << ","
                 << snapshot.no_ask << ","
                 << snapshot.no_ask_size << "\n";

    if (flush_each_write_) {
        pending_csv_flush_ = true;
    }
}

void Logger::consume_opportunity(const ArbOpportunity& opp) {
    note_live_event();
    if (!benchmark_window_active()) return;
    ++total_opps_;
    switch (opp.arb_kind) {
        case ArbKind::BUY_BOTH:
            ++buy_both_;
            break;
        case ArbKind::SELL_BOTH:
            ++sell_both_;
            break;
        case ArbKind::BUY_GROUP_YES:
            ++buy_group_;
            break;
        case ArbKind::SELL_GROUP_YES:
            ++sell_group_;
            break;
        case ArbKind::BUY_NO_SELL_OTHERS:
            ++buy_no_sell_others_;
            break;
        case ArbKind::SELL_NO_BUY_OTHERS:
            ++sell_no_buy_others_;
            break;
        case ArbKind::MAKER_BUY_BOTH:
            ++maker_buy_both_;
            break;
        case ArbKind::MAKER_SELL_BOTH:
            ++maker_sell_both_;
            break;
        case ArbKind::MAKER_BUY_GROUP_YES:
            ++maker_buy_group_;
            break;
        case ArbKind::MAKER_SELL_GROUP_YES:
            ++maker_sell_group_;
            break;
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
            ++maker_buy_no_sell_others_;
            break;
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
            ++maker_sell_no_buy_others_;
            break;
    }
    if (is_maker_arb_kind(opp.arb_kind)) {
        cumulative_maker_theoretical_pnl_ += opp.theoretical_pnl;
    }
    if (opp.counts_toward_paper_trade) {
        ++paper_trade_count_;
        cumulative_paper_pnl_ += opp.paper_trade_pnl;
        cumulative_paper_shares_ += static_cast<double>(opp.size_shares);
    }

    if (!csv_file_.is_open()) return;

    const double recv_parse_us = static_cast<double>(opp.t1_parse_done_ns - opp.t0_ws_recv_ns) / 1000.0;
    const double book_us = static_cast<double>(opp.t2_book_updated_ns - opp.t1_parse_done_ns) / 1000.0;
    const double arb_us = static_cast<double>(opp.t3_arb_checked_ns - opp.t2_book_updated_ns) / 1000.0;
    const double e2e_us = static_cast<double>(opp.t3_arb_checked_ns - opp.t0_ws_recv_ns) / 1000.0;

    csv_file_ << opp.t0_ws_recv_ns << ","
              << opp.contract_name << ","
              << arb_kind_name(opp.arb_kind) << ","
              << opp.ask_yes << ","
              << opp.ask_no << ","
              << opp.bid_yes << ","
              << opp.bid_no << ","
              << opp.cost_or_proceeds << ","
              << opp.raw_edge_bps << ","
              << opp.edge_bps << ","
              << opp.size_shares << ","
              << static_cast<unsigned>(opp.leg_count) << ","
              << opp.theoretical_pnl << ","
              << opp.t0_ws_recv_ns << ","
              << opp.t1_parse_done_ns << ","
              << opp.t2_book_updated_ns << ","
              << opp.t3_arb_checked_ns << ","
              << opp.t4_logged_ns << ","
              << recv_parse_us << ","
              << book_us << ","
              << arb_us << ","
              << e2e_us << "\n";

    if (flush_each_write_) {
        pending_csv_flush_ = true;
    }
}

void Logger::flush_pending_csv() {
    if (!flush_each_write_ || !pending_csv_flush_) return;
    if (csv_file_.is_open()) {
        csv_file_.flush();
    }
    if (near_miss_file_.is_open()) {
        near_miss_file_.flush();
    }
    if (replay_file_.is_open()) {
        replay_file_.flush();
    }
    pending_csv_flush_ = false;
}

void Logger::maybe_print_summary() {
    if (!metrics_enabled_) return;
    if (!benchmark_window_active()) return;

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - interval_start_).count();
    if (elapsed >= summary_interval_seconds_) {
        print_summary(static_cast<int>(elapsed));
        reset_interval();
        interval_start_ = now;
    }
}

void Logger::print_final_summary() {
    if (!metrics_enabled_) return;
    if (!benchmark_window_active()) {
        if (!warmup_started_) {
            std::printf("[LOGGER] Warmup window never started; no live events were observed\n");
        } else {
            std::printf("[LOGGER] Warmup window not completed; summary reflects no post-warmup samples\n");
        }
        std::fflush(stdout);
    }
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - interval_start_).count();
    print_summary(static_cast<int>(elapsed));
    if (csv_file_.is_open()) {
        csv_file_.flush();
        pending_csv_flush_ = false;
    }
    if (near_miss_file_.is_open()) {
        near_miss_file_.flush();
    }
    if (replay_file_.is_open()) {
        replay_file_.flush();
    }
}

void Logger::reset_interval() {
    lat_queue_.reset();
    lat_parse_.reset();
    lat_book_.reset();
    lat_arb_.reset();
    lat_e2e_.reset();
    lat_feed_ms_.reset();
    msg_sizes_.reset();

    interval_msgs_ = msg_count_;
    interval_books_ = book_count_;
    interval_price_changes_ = price_change_count_;
    interval_bbo_ = bbo_count_;
    interval_trades_ = trade_count_;
}

void Logger::print_summary(int interval_seconds) {
    const uint64_t imsg = msg_count_ - interval_msgs_;
    const uint64_t ibook = book_count_ - interval_books_;
    const uint64_t ipx = price_change_count_ - interval_price_changes_;
    const uint64_t ibbo = bbo_count_ - interval_bbo_;
    const uint64_t itrade = trade_count_ - interval_trades_;
    const double rate = interval_seconds > 0 ? static_cast<double>(imsg) / interval_seconds : 0.0;

    std::printf("\n");
    std::printf("╔══════════════════════════════════════════════════════════════════╗\n");
    std::printf("║  SUMMARY (%ds interval)                                        ║\n", interval_seconds);
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  Messages: %-6lu (%.1f/s)                                      ║\n", imsg, rate);
    std::printf("║  Books: %-6lu | PxChg: %-6lu | BBO: %-6lu | Trades: %-6lu ║\n",
                ibook, ipx, ibbo, itrade);
    std::printf("║  Total: %-6lu msgs | %-6lu books | %-6lu pxchg | %-6lu bbo ║\n",
                msg_count_, book_count_, price_change_count_, bbo_count_);
    std::printf("║         %-6lu trades                                          ║\n", trade_count_);
    std::printf("║  Arb checks: %-6lu | Signals: %-4lu | Paper: %-4lu          ║\n",
                arb_checks_, total_opps_, paper_trade_count_);
    std::printf("║  Same Mkt: BB:%-4lu SB:%-4lu | Group: BG:%-4lu SG:%-4lu     ║\n",
                buy_both_, sell_both_, buy_group_, sell_group_);
    std::printf("║  NoVsRest: BNS:%-4lu SNB:%-4lu                              ║\n",
                buy_no_sell_others_, sell_no_buy_others_);
    std::printf("║  Maker: MBB:%-4lu MSB:%-4lu | MBG:%-4lu MSG:%-4lu        ║\n",
                maker_buy_both_, maker_sell_both_, maker_buy_group_, maker_sell_group_);
    std::printf("║  MNoRest: MBN:%-4lu MSN:%-4lu                             ║\n",
                maker_buy_no_sell_others_, maker_sell_no_buy_others_);
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  LATENCY (μs)          min      avg      p50      p99    p99.9 ║\n");
    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");

    auto print_lat = [](const char* label, const LatencyTracker& tracker) {
        const auto s = tracker.compute();
        if (s.count == 0) {
            std::printf("║  %-18s   (no samples)                                    ║\n", label);
            return;
        }
        std::printf("║  %-18s %7.1f  %7.1f  %7.1f  %7.1f  %7.1f   ║\n",
                    label, s.min, s.avg, s.p50, s.p99, s.p999);
    };

    print_lat("Queue wait", lat_queue_);
    print_lat("Parse (frame)", lat_parse_);
    print_lat("Book upd (frame)", lat_book_);
    print_lat("Arb batch", lat_arb_);
    print_lat("End-to-end (t0→t3)", lat_e2e_);
    // Feed delivery is in MILLISECONDS (not µs) — it's network + their origin
    // processing, ~10,000x the in-process pipeline and the real geography lever.
    print_lat("Feed deliv (ms)", lat_feed_ms_);

    const auto sz = msg_sizes_.compute();
    if (sz.count > 0) {
        std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
        std::printf("║  Msg size (bytes)    %7.0f  %7.0f  %7.0f  %7.0f  %7.0f   ║\n",
                    sz.min, sz.avg, sz.p50, sz.p99, sz.p999);
    }

    if (edge_telemetry_enabled_) {
        const auto print_edges = [](const char* label, const EdgeTracker& tracker) {
            if (tracker.total == 0) {
                std::printf("║  %-18s   (no checks)                                    ║\n", label);
                return;
            }

            std::printf("║  %-18s <0:%-4lu 0-4:%-4lu 5-9:%-4lu 10-19:%-4lu 20+:%-4lu ║\n",
                        label,
                        tracker.raw_buckets[0],
                        tracker.raw_buckets[1],
                        tracker.raw_buckets[2],
                        tracker.raw_buckets[3],
                        tracker.raw_buckets[4]);
            std::printf("║  %-18s best raw=%-4d bps | best net=%-4d bps            ║\n",
                        "",
                        tracker.best_raw_bps,
                        tracker.best_net_bps);
        };

        std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
        std::printf("║  RAW EDGE BPS (cumulative)                                   ║\n");
        print_edges("BUY_BOTH", edge_buy_both_);
        print_edges("SELL_BOTH", edge_sell_both_);
        print_edges("BUY_GROUP_YES", edge_buy_group_);
        print_edges("SELL_GROUP_YES", edge_sell_group_);
        print_edges("BUY_NO_SELL_OTH", edge_buy_no_sell_others_);
        print_edges("SELL_NO_BUY_OTH", edge_sell_no_buy_others_);
        print_edges("MAKER_BUY_BOTH", edge_maker_buy_both_);
        print_edges("MAKER_SELL_BOTH", edge_maker_sell_both_);
        print_edges("MAKER_BUY_GROUP", edge_maker_buy_group_);
        print_edges("MAKER_SELL_GRP", edge_maker_sell_group_);
        print_edges("MAKER_BUY_NVR", edge_maker_buy_no_sell_others_);
        print_edges("MAKER_SELL_NVR", edge_maker_sell_no_buy_others_);
    }

    std::printf("╠══════════════════════════════════════════════════════════════════╣\n");
    std::printf("║  PAPER TRADING                                                  ║\n");
    std::printf("║  Paper P&L (TAKER): $%-10.2f | Shares: %-10.0f           ║\n",
                cumulative_paper_pnl_, cumulative_paper_shares_);
    std::printf("║  Maker theo excl.: $%-10.2f                                ║\n",
                cumulative_maker_theoretical_pnl_);
    std::printf("╚══════════════════════════════════════════════════════════════════╝\n\n");
    std::fflush(stdout);
}
