#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>
#include <chrono>

// Price stored as uint16_t × 1000 (e.g., $0.48 = 480)
using Price = uint16_t;
// Size stored as uint32_t whole units (share counts truncated to whole numbers)
using Size = uint32_t;
// Nanosecond timestamp
using NanoTime = uint64_t;

static constexpr Price PRICE_ONE = 1000;  // $1.00
static constexpr size_t PRICE_LEVEL_COUNT = PRICE_ONE + 1;
static constexpr int MAX_LEVELS = 50;

enum class ArbKind : uint8_t {
    BUY_BOTH = 0,
    SELL_BOTH = 1,
    BUY_GROUP_YES = 2,
    SELL_GROUP_YES = 3,
    BUY_NO_SELL_OTHERS = 4,
    SELL_NO_BUY_OTHERS = 5,
    MAKER_BUY_BOTH = 6,
    MAKER_SELL_BOTH = 7,
    MAKER_BUY_GROUP_YES = 8,
    MAKER_SELL_GROUP_YES = 9,
    MAKER_BUY_NO_SELL_OTHERS = 10,
    MAKER_SELL_NO_BUY_OTHERS = 11,
};

inline std::string_view arb_kind_name(ArbKind kind) noexcept {
    switch (kind) {
        case ArbKind::BUY_BOTH: return "BUY_BOTH";
        case ArbKind::SELL_BOTH: return "SELL_BOTH";
        case ArbKind::BUY_GROUP_YES: return "BUY_GROUP_YES";
        case ArbKind::SELL_GROUP_YES: return "SELL_GROUP_YES";
        case ArbKind::BUY_NO_SELL_OTHERS: return "BUY_NO_SELL_OTHERS";
        case ArbKind::SELL_NO_BUY_OTHERS: return "SELL_NO_BUY_OTHERS";
        case ArbKind::MAKER_BUY_BOTH: return "MAKER_BUY_BOTH";
        case ArbKind::MAKER_SELL_BOTH: return "MAKER_SELL_BOTH";
        case ArbKind::MAKER_BUY_GROUP_YES: return "MAKER_BUY_GROUP_YES";
        case ArbKind::MAKER_SELL_GROUP_YES: return "MAKER_SELL_GROUP_YES";
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS: return "MAKER_BUY_NO_SELL_OTHERS";
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS: return "MAKER_SELL_NO_BUY_OTHERS";
    }
    return "UNKNOWN";
}

inline bool is_maker_arb_kind(ArbKind kind) noexcept {
    switch (kind) {
        case ArbKind::MAKER_BUY_BOTH:
        case ArbKind::MAKER_SELL_BOTH:
        case ArbKind::MAKER_BUY_GROUP_YES:
        case ArbKind::MAKER_SELL_GROUP_YES:
        case ArbKind::MAKER_BUY_NO_SELL_OTHERS:
        case ArbKind::MAKER_SELL_NO_BUY_OTHERS:
            return true;
        default:
            return false;
    }
}

struct PriceLevel {
    Price price = 0;
    Size  size  = 0;
};

// 1024-bit occupancy bitmap (16 words) over the price grid [0,1000], so the best
// bid/ask is a few word ops + clz/ctz instead of a linear scan of up to 1001
// price slots on every price_change. Kept in lockstep with *_size_by_price.
static constexpr size_t PRICE_OCC_WORDS = (PRICE_LEVEL_COUNT + 63) / 64;

struct alignas(64) Orderbook {
    PriceLevel bids[MAX_LEVELS];
    PriceLevel asks[MAX_LEVELS];
    Size       bid_size_by_price[PRICE_LEVEL_COUNT] = {};
    Size       ask_size_by_price[PRICE_LEVEL_COUNT] = {};
    uint32_t   bid_epoch_by_price[PRICE_LEVEL_COUNT] = {};
    uint32_t   ask_epoch_by_price[PRICE_LEVEL_COUNT] = {};
    uint64_t   bid_occ[PRICE_OCC_WORDS] = {};
    uint64_t   ask_occ[PRICE_OCC_WORDS] = {};
    uint32_t   bid_epoch = 1;
    uint32_t   ask_epoch = 1;
    uint8_t    bid_count     = 0;
    uint8_t    ask_count     = 0;
    Price      best_bid      = 0;
    Price      best_ask      = 0;
    Size       best_bid_size = 0;
    Size       best_ask_size = 0;
    bool       has_snapshot  = false;
    uint64_t   timestamp     = 0;     // exchange timestamp
    NanoTime   local_update_ns = 0;   // our clock when processed
    uint64_t   touched_frame_id = 0;
    Price      tick_thou     = 0;     // min price increment in thousandths (0 = unknown)
    uint64_t   last_hash     = 0;     // FNV-1a of last book/price_change hash (gap detection)
    // ACR (anti-cancel-race): EWMA of |Δmid| (×2 space) to widen quotes when the
    // market moves fast, plus the last mid×2 for the delta.
    uint32_t   acr_last_mid2 = 0;
    float      acr_vol_thou  = 0.0f;
    // Adaptive quote throttle (live rate-limit / queue-priority preservation):
    // last reconcile time + current adaptive cadence for this token's book.
    uint64_t   thr_last_quote_ns = 0;
    uint32_t   thr_interval_ms   = 0;
};

struct Contract {
    std::string condition_id;
    std::string asset_name;
    std::string token_id_yes;
    std::string token_id_no;
    int         taker_fee_bps_override = -1;
    bool        market_metadata_loaded = false;
    bool        fee_schedule_enabled = false;
    bool        fee_metadata_valid = false;  // true only if fee schedule was positively parsed
    double      fee_rate = 0.0;
    double      fee_rebate_rate = 0.0;
    bool        fee_taker_only = true;
    int         fee_exponent = 0;
    int         base_fee_bps = -1;
    Price       tick_thou = 0;          // gamma orderPriceMinTickSize, thousandths (seeds books)
    // Liquidity-rewards config (CLOB /markets/{cid} -> rewards{}); see rewards.hpp.
    bool        reward_active = false;          // rates != null (emitting rewards now)
    Price       reward_max_spread_thou = 0;     // max_spread cents -> thousandths
    Size        reward_min_size = 0;            // min qualifying order size (shares)
    double      reward_daily_rate_usd = 0.0;    // sum of rewards_daily_rate
    bool        neg_risk = false;
    std::string event_slug;
    std::string event_title;
    uint64_t    touched_frame_id = 0;
    uint64_t    total_updates = 0;
    uint64_t    yes_updates = 0;
    uint64_t    no_updates = 0;
    Orderbook   book_yes;
    Orderbook   book_no;
};

struct ArbOpportunity {
    NanoTime    t0_ws_recv_ns      = 0;
    NanoTime    t1_parse_done_ns   = 0;
    NanoTime    t2_book_updated_ns = 0;
    NanoTime    t3_arb_checked_ns  = 0;
    NanoTime    t4_logged_ns       = 0;
    std::string_view contract_name;
    ArbKind      arb_kind = ArbKind::BUY_BOTH;
    Price       ask_yes    = 0;
    Price       ask_no     = 0;
    Price       bid_yes    = 0;
    Price       bid_no     = 0;
    uint16_t    cost_or_proceeds = 0;
    int16_t     edge_bps   = 0;
    int16_t     raw_edge_bps = 0;
    Size        size_shares = 0;
    uint8_t     leg_count  = 2;
    double      theoretical_pnl = 0.0;
    double      paper_trade_pnl = 0.0;
    bool        counts_toward_paper_trade = false;
};

struct EdgeTelemetrySample {
    NanoTime         sample_time_ns = 0;
    std::string_view label;
    ArbKind          arb_kind = ArbKind::BUY_BOTH;
    uint16_t         reference_value = 0;
    uint8_t          leg_count = 0;
    int16_t          raw_edge_bps = 0;
    int16_t          net_edge_bps = 0;
    bool             valid = false;
};

struct ReplaySnapshot {
    uint64_t         frame_id = 0;
    NanoTime         t0_ws_recv_ns = 0;
    NanoTime         t1_parse_start_ns = 0;
    NanoTime         t2_parse_done_ns = 0;
    NanoTime         t3_books_done_ns = 0;
    uint32_t         frame_bytes = 0;
    uint16_t         book_events = 0;
    uint16_t         price_change_events = 0;
    uint16_t         bbo_events = 0;
    uint16_t         trade_events = 0;
    std::string_view event_key;
    std::string_view event_label;
    std::string_view contract_name;
    uint8_t          leg_index = 0;
    uint8_t          leg_count = 0;
    bool             touched_in_frame = false;
    uint64_t         yes_exchange_ts = 0;
    uint64_t         no_exchange_ts = 0;
    Price            yes_bid = 0;
    Size             yes_bid_size = 0;
    Price            yes_ask = 0;
    Size             yes_ask_size = 0;
    Price            no_bid = 0;
    Size             no_bid_size = 0;
    Price            no_ask = 0;
    Size             no_ask_size = 0;
};

struct Config {
    std::string websocket_host = "ws-subscriptions-clob.polymarket.com";
    std::string websocket_path = "/ws/market";
    uint16_t    websocket_port = 443;
    int         min_edge_threshold_bps = 0;
    int         taker_fee_bps = 100;
    int         summary_interval_seconds = 60;
    std::string log_file = "logs/arb_log.csv";
    std::string near_miss_log_file = "logs/near_miss.csv";
    std::string replay_log_file = "logs/replay_state.csv";
    int         warmup_seconds = 0;
    int         ping_interval_seconds = 15;
    int         stale_feed_timeout_seconds = 30;
    int         reconnect_max_delay_seconds = 30;
    size_t      message_queue_capacity = 128;
    size_t      metrics_queue_capacity = 8192;
    size_t      opportunity_queue_capacity = 1024;
    size_t      replay_queue_capacity = 8192;
    size_t      active_market_report_limit = 10;
    bool        custom_feature_enabled = true;
    bool        initial_dump = true;
    bool        metrics_enabled = true;
    bool        edge_telemetry_enabled = true;
    bool        replay_logging_enabled = true;
    bool        hot_path_logging = false;
    bool        flush_csv_each_write = false;
    bool        fetch_market_metadata = true;
    std::string market_metadata_cache_file = "logs/market_metadata_cache.tsv";
    size_t      metadata_fetch_threads = 4;
    bool        enable_group_arbitrage = true;
    bool        auto_detect_exhaustive_groups = true;
    bool        maker_arb_enabled = true;
    // Liquidity-rewards shadow executor (no keys/sends; logs intended orders).
    bool        shadow_executor_enabled = false;
    bool        shadow_executor_verbose = false;
    // Execution mode: "shadow" (log only), "mocklive" (build v2 order + EIP-712
    // digest, no key/sign/send), or "live" (gated, not built). Selects the
    // gateway on the cancel-sender thread; the OMS/ACR/risk path is mode-agnostic.
    std::string exec_mode = "shadow";
    std::string live_maker_address  = "0x0000000000000000000000000000000000000000";
    std::string live_signer_address = "0x0000000000000000000000000000000000000000";
    int         live_signature_type = 0;          // 0 EOA, 1 POLY_PROXY, 2 POLY_GNOSIS_SAFE
    // ── Live order path (exec_mode="live"). API creds + EOA key are read from the
    // ENVIRONMENT only (PM_API_KEY/PM_API_SECRET/PM_API_PASSPHRASE/PM_SIGNER_KEY) —
    // NEVER from this config file, never logged. `live_arm` is a hard safety latch:
    // unless true, live mode signs+builds but does NOT POST (dry-run). The gateway
    // also self-gates on a GET /version preflight (must be the v2 it was built for).
    std::string live_host = "clob.polymarket.com";
    std::string live_port = "443";
    bool        live_arm = false;                 // false => dry-run (no POST), the safe default
    std::string live_order_type = "GTC";          // GTC resting maker quote (rewards)
    bool        live_cancel_all_on_start = true;   // clear any orphaned resting orders first
    int         live_order_version = 2;            // expected CLOB order version (preflight checks /version)
    // exec_mode="relay": forward orders to the local Python order-gateway connector
    // (tools/order_gateway_server.py), which signs sigType-3 (deposit wallet) + POSTs.
    // The bearer token comes from $ORDER_GW_TOKEN (env), never the config. live_arm
    // still gates sending (false => the relay builds requests but does not POST).
    std::string relay_host = "127.0.0.1";
    int         relay_port = 8765;
    std::string near_miss_live_log_file = "logs/near_miss_live.csv";
    uint32_t    reward_quote_size = 0;            // 0 => use each market's min_size
    uint32_t    reward_target_offset_thou = 0;    // 0 => tightest (1 tick)
    double      risk_max_gross_notional_usd = 1000.0;
    double      risk_max_position_shares = 5000.0;
    uint32_t    risk_max_open_orders_total = 64;
    double      risk_pusd_allowance_usd = 0.0;    // simulated pUSD allowance (0 = unlimited)
    // Adaptive quote throttle (suppress re-pricing churn between book moves).
    bool        quote_throttle_enabled = false;
    uint32_t    quote_throttle_min_ms = 5;
    uint32_t    quote_throttle_max_ms = 100;
    double      quote_throttle_vol_hot_thou = 2.0; // acr_vol_thou above this => volatile
    // Quote telemetry capture (Roadmap #1: groundwork for adverse-selection net).
    bool        quote_telemetry_enabled = false;
    std::string quote_telemetry_log_file = "logs/quote_telemetry.csv";
    size_t      quote_telemetry_queue_capacity = 8192;
    // Live-safety: feed stale > this => cancel every resting order + halt new ones
    // (dead-man's-switch); re-arms when data returns. 0 = disabled.
    int         dead_mans_switch_seconds = 0;
    // Markets rotate (rates:null) mid-session; re-fetch reward config every N s and
    // apply via a single-writer (parser) update ring. 0 = fetch once at startup.
    int         reward_refresh_seconds = 0;
    size_t      reward_update_queue_capacity = 64;
    // ACR (anti-cancel-race): fast defensive cancels + skew/vol quoting.
    bool        acr_enabled = true;
    int         acr_stale_drift_ticks = 1;        // cancel when mid drifts this many ticks against a quote
    double      acr_inv_skew_per_share_thou = 0.0;// quote shift per share of inventory (flattening)
    double      acr_vol_widen_k = 0.0;            // widen each side by k * mid-vol EWMA
    size_t      command_queue_capacity = 1024;    // OMS -> sender-thread ring
    int         sender_cpu = -1;                  // cancel-sender thread pinning
    int         sender_priority = 0;              // cancel-sender RT priority (SCHED_FIFO)
    // Sender idle behavior. 0 = pure busy-poll (lowest latency; pegs a core — the
    // profile showed it spins a full core for ~12 events in 5 min). >0 = spin-then-
    // park: busy-poll briefly after activity, then sleep this many µs once idle,
    // freeing the core. Cancels are rare and latency-tolerant vs the ms network, so
    // use >0 on small/shared boxes; keep 0 on a dedicated isolated core.
    uint32_t    sender_park_after_idle_us = 0;
    // WebSocket transport A/B knobs (network-bound; measure, don't assume).
    bool        ws_tls13_enabled = true;          // generic TLS context (negotiates up to 1.3)
    bool        ws_permessage_deflate = false;    // negotiate WS compression (helps bootstrap burst)
    int         pin_thread_cpu = -1;
    int         receiver_cpu = 2;
    int         parser_cpu = 4;
    int         logger_cpu = 6;
    bool        lock_memory = false;
    size_t      prefault_stack_kb = 64;
    int         realtime_priority = 0;
    int         receiver_priority = 0;
    int         parser_priority = 0;
    int         logger_priority = 0;
    struct ConfiguredGroup {
        std::string key;
        std::string display_name;
        std::vector<std::string> condition_ids;
        bool exhaustive = true;
    };
    std::vector<Contract> contracts;
    std::vector<ConfiguredGroup> configured_groups;
};

struct ContractGroup {
    std::string key;
    std::string display_name;
    std::vector<Contract*> contracts;
    bool exhaustive = false;
    uint64_t touched_frame_id = 0;
};

inline NanoTime now_ns() {
    auto tp = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}

// Wall-clock (CLOCK_REALTIME) nanoseconds since the Unix epoch. now_ns() is
// steady_clock and CANNOT be compared to Polymarket's exchange timestamps (ms
// epoch); this can. Used only to measure feed-delivery latency
// (recv_wall − event.timestamp), i.e. the geography lever — not on the hot
// arb path, where steady_clock deltas stay correct.
inline NanoTime now_realtime_ns() {
    auto tp = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}
