#include "types.hpp"
#include "websocket.hpp"
#include "parser.hpp"
#include "orderbook.hpp"
#include "arbitrage.hpp"
#include "logger.hpp"
#include "market_metadata.hpp"
#include "rewards.hpp"
#include "oms.hpp"
#include "acr.hpp"
#include "throttle.hpp"
#include "mock_live_gateway.hpp"
#include "runtime.hpp"
#include "pipeline.hpp"

#include <simdjson.h>

#include <iostream>
#include <fstream>
#include <csignal>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <atomic>
#include <algorithm>
#include <thread>
#include <memory>
#include <cstdio>
#include <cinttypes>
#include <cmath>
#include <unistd.h>

static std::atomic<bool> g_running{true};

struct TokenRoute {
    Contract* contract = nullptr;
    bool is_yes = false;
};

static inline bool has_usable_book(const Orderbook& book) {
    return book.has_snapshot && book.best_bid > 0 && book.best_ask > 0;
}

static inline bool has_usable_yes_book(const Contract& contract) {
    return contract.book_yes.has_snapshot &&
           contract.book_yes.best_bid > 0 &&
           contract.book_yes.best_ask > 0;
}

static inline bool contract_ready_for_arb(const Contract& contract) {
    return has_usable_book(contract.book_yes) && has_usable_book(contract.book_no);
}

static inline bool group_ready_for_arb(const ContractGroup& group) {
    if (!group.exhaustive || group.contracts.size() < 2) return false;
    for (const Contract* contract : group.contracts) {
        if (!has_usable_yes_book(*contract)) {
            return false;
        }
    }
    return true;
}

template <typename T>
void update_atomic_max(std::atomic<T>& target, T value) {
    T current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(
               current, value, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
}

void signal_handler(int sig) {
    g_running.store(false, std::memory_order_relaxed);
    constexpr char kSignalMsg[] = "\n[SIGNAL] Shutdown requested\n";
    (void)sig;
    (void)!::write(STDERR_FILENO, kSignalMsg, sizeof(kSignalMsg) - 1);
}

// Load config from JSON file
Config load_config(const std::string& path) {
    Config config;

    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[CONFIG] Cannot open " << path << ", using defaults\n";
        return config;
    }

    std::string json_str((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json_str);
    auto doc = parser.iterate(padded);

    std::string_view sv;
    int64_t iv;
    bool bv;
    double dv;
    if (!doc["min_edge_threshold_bps"].get_int64().get(iv))
        config.min_edge_threshold_bps = (int)iv;
    if (!doc["taker_fee_bps"].get_int64().get(iv))
        config.taker_fee_bps = (int)iv;
    if (!doc["summary_interval_seconds"].get_int64().get(iv))
        config.summary_interval_seconds = (int)iv;
    if (!doc["warmup_seconds"].get_int64().get(iv))
        config.warmup_seconds = (int)iv;
    if (!doc["ping_interval_seconds"].get_int64().get(iv))
        config.ping_interval_seconds = (int)iv;
    if (!doc["stale_feed_timeout_seconds"].get_int64().get(iv))
        config.stale_feed_timeout_seconds = (int)iv;
    if (!doc["reconnect_max_delay_seconds"].get_int64().get(iv))
        config.reconnect_max_delay_seconds = (int)iv;
    if (!doc["message_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.message_queue_capacity = (size_t)iv;
    if (!doc["metrics_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.metrics_queue_capacity = (size_t)iv;
    if (!doc["opportunity_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.opportunity_queue_capacity = (size_t)iv;
    if (!doc["replay_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.replay_queue_capacity = (size_t)iv;
    if (!doc["active_market_report_limit"].get_int64().get(iv) && iv > 0)
        config.active_market_report_limit = (size_t)iv;
    if (!doc["custom_feature_enabled"].get_bool().get(bv))
        config.custom_feature_enabled = bv;
    if (!doc["initial_dump"].get_bool().get(bv))
        config.initial_dump = bv;
    if (!doc["metrics_enabled"].get_bool().get(bv))
        config.metrics_enabled = bv;
    if (!doc["edge_telemetry_enabled"].get_bool().get(bv))
        config.edge_telemetry_enabled = bv;
    if (!doc["log_file"].get_string().get(sv))
        config.log_file = std::string(sv);
    if (!doc["near_miss_log_file"].get_string().get(sv))
        config.near_miss_log_file = std::string(sv);
    if (!doc["replay_log_file"].get_string().get(sv))
        config.replay_log_file = std::string(sv);
    if (!doc["replay_logging_enabled"].get_bool().get(bv))
        config.replay_logging_enabled = bv;
    if (!doc["hot_path_logging"].get_bool().get(bv))
        config.hot_path_logging = bv;
    if (!doc["flush_csv_each_write"].get_bool().get(bv))
        config.flush_csv_each_write = bv;
    if (!doc["fetch_market_metadata"].get_bool().get(bv))
        config.fetch_market_metadata = bv;
    if (!doc["market_metadata_cache_file"].get_string().get(sv))
        config.market_metadata_cache_file = std::string(sv);
    if (!doc["metadata_fetch_threads"].get_int64().get(iv) && iv > 0)
        config.metadata_fetch_threads = static_cast<size_t>(iv);
    if (!doc["enable_group_arbitrage"].get_bool().get(bv))
        config.enable_group_arbitrage = bv;
    if (!doc["auto_detect_exhaustive_groups"].get_bool().get(bv))
        config.auto_detect_exhaustive_groups = bv;
    if (!doc["maker_arb_enabled"].get_bool().get(bv))
        config.maker_arb_enabled = bv;
    if (!doc["shadow_executor_enabled"].get_bool().get(bv))
        config.shadow_executor_enabled = bv;
    if (!doc["shadow_executor_verbose"].get_bool().get(bv))
        config.shadow_executor_verbose = bv;
    if (!doc["reward_quote_size"].get_int64().get(iv))
        config.reward_quote_size = (uint32_t)iv;
    if (!doc["reward_target_offset_thou"].get_int64().get(iv))
        config.reward_target_offset_thou = (uint32_t)iv;
    if (!doc["risk_max_gross_notional_usd"].get_double().get(dv))
        config.risk_max_gross_notional_usd = dv;
    if (!doc["risk_max_position_shares"].get_double().get(dv))
        config.risk_max_position_shares = dv;
    if (!doc["risk_max_open_orders_total"].get_int64().get(iv))
        config.risk_max_open_orders_total = (uint32_t)iv;
    if (!doc["acr_enabled"].get_bool().get(bv))
        config.acr_enabled = bv;
    if (!doc["acr_stale_drift_ticks"].get_int64().get(iv))
        config.acr_stale_drift_ticks = (int)iv;
    if (!doc["acr_inv_skew_per_share_thou"].get_double().get(dv))
        config.acr_inv_skew_per_share_thou = dv;
    if (!doc["acr_vol_widen_k"].get_double().get(dv))
        config.acr_vol_widen_k = dv;
    if (!doc["command_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.command_queue_capacity = (size_t)iv;
    if (!doc["sender_cpu"].get_int64().get(iv))
        config.sender_cpu = (int)iv;
    if (!doc["sender_priority"].get_int64().get(iv))
        config.sender_priority = (int)iv;
    // Execution mode + (mock) live signer identity.
    if (!doc["exec_mode"].get_string().get(sv))
        config.exec_mode = std::string(sv);
    if (!doc["live_maker_address"].get_string().get(sv))
        config.live_maker_address = std::string(sv);
    if (!doc["live_signer_address"].get_string().get(sv))
        config.live_signer_address = std::string(sv);
    if (!doc["live_signature_type"].get_int64().get(iv))
        config.live_signature_type = (int)iv;
    if (!doc["near_miss_live_log_file"].get_string().get(sv))
        config.near_miss_live_log_file = std::string(sv);
    if (!doc["risk_pusd_allowance_usd"].get_double().get(dv))
        config.risk_pusd_allowance_usd = dv;
    // Adaptive quote throttle.
    if (!doc["quote_throttle_enabled"].get_bool().get(bv))
        config.quote_throttle_enabled = bv;
    if (!doc["quote_throttle_min_ms"].get_int64().get(iv))
        config.quote_throttle_min_ms = (uint32_t)iv;
    if (!doc["quote_throttle_max_ms"].get_int64().get(iv))
        config.quote_throttle_max_ms = (uint32_t)iv;
    if (!doc["quote_throttle_vol_hot_thou"].get_double().get(dv))
        config.quote_throttle_vol_hot_thou = dv;
    // Quote telemetry capture.
    if (!doc["quote_telemetry_enabled"].get_bool().get(bv))
        config.quote_telemetry_enabled = bv;
    if (!doc["quote_telemetry_log_file"].get_string().get(sv))
        config.quote_telemetry_log_file = std::string(sv);
    if (!doc["quote_telemetry_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.quote_telemetry_queue_capacity = (size_t)iv;
    // Live-safety + reward-rotation.
    if (!doc["dead_mans_switch_seconds"].get_int64().get(iv))
        config.dead_mans_switch_seconds = (int)iv;
    if (!doc["reward_refresh_seconds"].get_int64().get(iv))
        config.reward_refresh_seconds = (int)iv;
    if (!doc["reward_update_queue_capacity"].get_int64().get(iv) && iv > 0)
        config.reward_update_queue_capacity = (size_t)iv;
    // WebSocket transport A/B.
    if (!doc["ws_tls13_enabled"].get_bool().get(bv))
        config.ws_tls13_enabled = bv;
    if (!doc["ws_permessage_deflate"].get_bool().get(bv))
        config.ws_permessage_deflate = bv;
    if (!doc["pin_thread_cpu"].get_int64().get(iv))
        config.pin_thread_cpu = (int)iv;
    if (!doc["receiver_cpu"].get_int64().get(iv))
        config.receiver_cpu = (int)iv;
    if (!doc["parser_cpu"].get_int64().get(iv))
        config.parser_cpu = (int)iv;
    if (!doc["logger_cpu"].get_int64().get(iv))
        config.logger_cpu = (int)iv;
    if (!doc["lock_memory"].get_bool().get(bv))
        config.lock_memory = bv;
    if (!doc["prefault_stack_kb"].get_int64().get(iv))
        config.prefault_stack_kb = (size_t)iv;
    if (!doc["realtime_priority"].get_int64().get(iv))
        config.realtime_priority = (int)iv;
    if (!doc["receiver_priority"].get_int64().get(iv))
        config.receiver_priority = (int)iv;
    if (!doc["parser_priority"].get_int64().get(iv))
        config.parser_priority = (int)iv;
    if (!doc["logger_priority"].get_int64().get(iv))
        config.logger_priority = (int)iv;

    simdjson::ondemand::array groups;
    if (!doc["groups"].get_array().get(groups)) {
        for (auto elem : groups) {
            Config::ConfiguredGroup group;
            simdjson::ondemand::object obj;
            if (elem.get_object().get(obj)) continue;

            std::string_view s;
            if (!obj["key"].get_string().get(s)) group.key = std::string(s);
            if (!obj["display_name"].get_string().get(s)) group.display_name = std::string(s);
            if (!obj["exhaustive"].get_bool().get(bv)) group.exhaustive = bv;

            simdjson::ondemand::array condition_ids;
            if (!obj["condition_ids"].get_array().get(condition_ids)) {
                for (auto condition_elem : condition_ids) {
                    if (!condition_elem.get_string().get(s)) {
                        group.condition_ids.emplace_back(s);
                    }
                }
            }

            if (group.display_name.empty()) {
                group.display_name = group.key;
            }
            if (!group.condition_ids.empty()) {
                config.configured_groups.push_back(std::move(group));
            }
        }
    }

    simdjson::ondemand::array contracts;
    if (!doc["contracts"].get_array().get(contracts)) {
        for (auto elem : contracts) {
            Contract c;
            simdjson::ondemand::object obj;
            if (elem.get_object().get(obj)) continue;

            std::string_view s;
            if (!obj["name"].get_string().get(s)) c.asset_name = std::string(s);
            if (!obj["condition_id"].get_string().get(s)) c.condition_id = std::string(s);
            if (!obj["token_id_yes"].get_string().get(s)) c.token_id_yes = std::string(s);
            if (!obj["token_id_no"].get_string().get(s)) c.token_id_no = std::string(s);
            if (!obj["taker_fee_bps_override"].get_int64().get(iv)) c.taker_fee_bps_override = (int)iv;

            config.contracts.push_back(std::move(c));
        }
    }

    printf("[CONFIG] Loaded %zu contracts from %s\n", config.contracts.size(), path.c_str());
    return config;
}

int main(int argc, char* argv[]) {
    // Signal handling
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Load config
    std::string config_path = "config.json";
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "--config" && i + 1 < argc) {
            config_path = argv[i + 1];
            i++;
        }
    }

    Config config = load_config(config_path);
    apply_process_runtime_tuning(config);

    if (config.contracts.empty()) {
        std::cerr << "[ERROR] No contracts configured. Add contracts to config.json\n";
        return 1;
    }

    std::vector<ContractGroup> contract_groups;
    std::unordered_map<Contract*, std::vector<ContractGroup*>> contract_group_routes;
    std::unordered_map<std::string_view, Contract*> contracts_by_condition_id;
    contracts_by_condition_id.reserve(config.contracts.size());
    for (auto& contract : config.contracts) {
        contracts_by_condition_id.emplace(contract.condition_id, &contract);
    }
    std::unordered_set<std::string> seen_group_keys;

    if (config.enable_group_arbitrage) {
        contract_groups.reserve(config.configured_groups.size());
        for (const auto& configured_group : config.configured_groups) {
            ContractGroup group;
            group.key = configured_group.key.empty()
                ? configured_group.display_name
                : configured_group.key;
            group.display_name = configured_group.display_name.empty()
                ? group.key
                : configured_group.display_name;
            group.exhaustive = configured_group.exhaustive;

            bool missing_contract = false;
            for (const std::string& condition_id : configured_group.condition_ids) {
                const auto it = contracts_by_condition_id.find(condition_id);
                if (it == contracts_by_condition_id.end()) {
                    std::fprintf(stderr, "[GROUP] %s | missing contract condition_id=%s\n",
                                 group.display_name.c_str(), condition_id.c_str());
                    missing_contract = true;
                    break;
                }
                group.contracts.push_back(it->second);
            }

            if (missing_contract || group.contracts.size() < 2 || !group.exhaustive) {
                continue;
            }
            if (!seen_group_keys.emplace(group.key).second) {
                continue;
            }
            contract_groups.push_back(std::move(group));
        }
    }

    if (config.fetch_market_metadata) {
        struct MetadataFetchResult {
            bool ok = false;
            MarketMetadata metadata;
            std::string error;
        };
        struct EventCountFetchResult {
            bool ok = false;
            size_t market_count = 0;
            std::string error;
        };

        auto apply_metadata = [](Contract& contract, MarketMetadata metadata) {
            contract.fee_schedule_enabled = metadata.fees_enabled;
            contract.fee_metadata_valid = metadata.fee_schedule_valid;
            contract.fee_rate = metadata.fee_rate;
            contract.fee_rebate_rate = metadata.fee_rebate_rate;
            contract.fee_taker_only = metadata.fee_taker_only;
            contract.fee_exponent = metadata.fee_exponent;
            contract.base_fee_bps = metadata.base_fee_bps;
            contract.neg_risk = metadata.neg_risk;
            contract.market_metadata_loaded = true;
            contract.event_slug = std::move(metadata.event_slug);
            contract.event_title = std::move(metadata.event_title);
            // Seed per-market tick (gamma orderPriceMinTickSize, e.g. 0.001 -> 1
            // thousandth) so maker quoting is grid-aware before the first book.
            if (metadata.min_tick_size > 0.0) {
                const Price tick = static_cast<Price>(std::llround(metadata.min_tick_size * 1000.0));
                contract.tick_thou = tick;
                contract.book_yes.tick_thou = tick;
                contract.book_no.tick_thou = tick;
            }
        };

        const auto metadata_phase_start = std::chrono::steady_clock::now();
        MarketMetadataCache metadata_cache;
        if (!config.market_metadata_cache_file.empty()) {
            std::string cache_error;
            if (!load_market_metadata_cache(config.market_metadata_cache_file, metadata_cache, cache_error)) {
                std::fprintf(stderr, "[META] cache load failed: %s\n", cache_error.c_str());
            }
        }

        size_t metadata_cache_hits = 0;
        size_t metadata_fetched = 0;
        size_t metadata_failures = 0;
        std::vector<size_t> metadata_missing_indices;
        metadata_missing_indices.reserve(config.contracts.size());

        for (size_t i = 0; i < config.contracts.size(); ++i) {
            Contract& c = config.contracts[i];
            const auto cache_it = metadata_cache.markets_by_token.find(c.token_id_yes);
            if (cache_it != metadata_cache.markets_by_token.end()) {
                apply_metadata(c, cache_it->second);
                ++metadata_cache_hits;
            } else {
                metadata_missing_indices.push_back(i);
            }
        }

        if (!metadata_missing_indices.empty()) {
            std::atomic<size_t> next_index{0};
            std::vector<MetadataFetchResult> results(config.contracts.size());
            const size_t worker_count =
                std::min(std::max<size_t>(1, config.metadata_fetch_threads), metadata_missing_indices.size());
            std::vector<std::thread> workers;
            workers.reserve(worker_count);

            for (size_t worker_id = 0; worker_id < worker_count; ++worker_id) {
                workers.emplace_back([&] {
                    MarketMetadataClient client;
                    while (true) {
                        const size_t pos = next_index.fetch_add(1, std::memory_order_relaxed);
                        if (pos >= metadata_missing_indices.size()) break;

                        const size_t contract_index = metadata_missing_indices[pos];
                        Contract& contract = config.contracts[contract_index];
                        auto& result = results[contract_index];
                        result.ok = client.fetch_market_by_token(contract.token_id_yes, result.metadata, result.error);
                    }
                });
            }

            for (auto& worker : workers) {
                worker.join();
            }

            for (const size_t contract_index : metadata_missing_indices) {
                Contract& contract = config.contracts[contract_index];
                const auto& result = results[contract_index];
                if (!result.ok) {
                    ++metadata_failures;
                    std::fprintf(stderr, "[META] %s | market lookup failed: %s\n",
                                 contract.asset_name.c_str(), result.error.c_str());
                    continue;
                }

                apply_metadata(contract, result.metadata);
                metadata_cache.markets_by_token[contract.token_id_yes] = result.metadata;
                ++metadata_fetched;
            }
        }

        const auto metadata_phase_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - metadata_phase_start).count();
        std::printf("[META] Summary | contracts=%zu cache_hits=%zu fetched=%zu failed=%zu threads=%zu elapsed=%.2fms\n",
                    config.contracts.size(),
                    metadata_cache_hits,
                    metadata_fetched,
                    metadata_failures,
                    std::max<size_t>(1, config.metadata_fetch_threads),
                    metadata_phase_ms);

        for (auto& c : config.contracts) {
            std::printf("[META] %s | event=%s | negRisk=%s | fees=%s",
                        c.asset_name.c_str(),
                        c.event_slug.empty() ? "n/a" : c.event_slug.c_str(),
                        c.neg_risk ? "true" : "false",
                        c.fee_schedule_enabled ? "dynamic" : (c.market_metadata_loaded ? "none" : "fallback"));
            if (c.fee_schedule_enabled) {
                std::printf(" | rate=%.4f exp=%d", c.fee_rate, c.fee_exponent);
            } else if (c.market_metadata_loaded) {
                std::printf(" | rate=0");
            } else if (c.taker_fee_bps_override >= 0) {
                std::printf(" | override=%d bps", c.taker_fee_bps_override);
            } else {
                std::printf(" | default=%d bps", config.taker_fee_bps);
            }
            std::printf("\n");
        }

        // Fetch liquidity-rewards config from the CLOB (read-only) for the shadow
        // executor. Sequential startup calls — fine for the small market count.
        if (config.shadow_executor_enabled) {
            ClobRewardsClient rewards_client;
            size_t active = 0;
            for (auto& c : config.contracts) {
                if (c.condition_id.empty()) continue;
                RewardConfigRaw rc;
                std::string err;
                if (!rewards_client.fetch(c.condition_id, rc, err)) {
                    std::fprintf(stderr, "[REWARDS] fetch failed %s: %s\n",
                                 c.asset_name.c_str(), err.c_str());
                    continue;
                }
                c.reward_active = rc.active;
                c.reward_max_spread_thou = static_cast<Price>(rc.max_spread_thou);
                c.reward_min_size = static_cast<Size>(rc.min_size);
                c.reward_daily_rate_usd = rc.daily_rate_usd;
                if (rc.active) {
                    ++active;
                    std::printf("[REWARDS] %s | active | max_spread=%.3f min_size=%u daily=$%.0f\n",
                                c.asset_name.c_str(),
                                static_cast<double>(rc.max_spread_thou) / 1000.0,
                                static_cast<unsigned>(rc.min_size), rc.daily_rate_usd);
                }
            }
            std::printf("[REWARDS] %zu/%zu markets currently emitting rewards\n",
                        active, config.contracts.size());
        }

        if (config.enable_group_arbitrage && config.auto_detect_exhaustive_groups) {
            const auto group_phase_start = std::chrono::steady_clock::now();
            std::unordered_map<std::string, std::vector<Contract*>> contracts_by_event;
            for (auto& c : config.contracts) {
                if (c.neg_risk && !c.event_slug.empty()) {
                    contracts_by_event[c.event_slug].push_back(&c);
                }
            }

            size_t event_cache_hits = 0;
            size_t event_fetched = 0;
            size_t event_failures = 0;
            std::unordered_map<std::string, size_t> event_market_counts;
            event_market_counts.reserve(contracts_by_event.size());
            std::vector<std::string> missing_event_slugs;
            missing_event_slugs.reserve(contracts_by_event.size());

            for (const auto& [event_slug, members] : contracts_by_event) {
                if (members.size() < 2) continue;
                const auto cache_it = metadata_cache.event_market_counts.find(event_slug);
                if (cache_it != metadata_cache.event_market_counts.end()) {
                    event_market_counts.emplace(event_slug, cache_it->second);
                    ++event_cache_hits;
                } else {
                    missing_event_slugs.push_back(event_slug);
                }
            }

            if (!missing_event_slugs.empty()) {
                std::atomic<size_t> next_event_index{0};
                std::vector<EventCountFetchResult> results(missing_event_slugs.size());
                const size_t worker_count =
                    std::min(std::max<size_t>(1, config.metadata_fetch_threads), missing_event_slugs.size());
                std::vector<std::thread> workers;
                workers.reserve(worker_count);

                for (size_t worker_id = 0; worker_id < worker_count; ++worker_id) {
                    workers.emplace_back([&] {
                        MarketMetadataClient client;
                        while (true) {
                            const size_t pos = next_event_index.fetch_add(1, std::memory_order_relaxed);
                            if (pos >= missing_event_slugs.size()) break;

                            auto& result = results[pos];
                            result.ok = client.fetch_event_market_count(
                                missing_event_slugs[pos], result.market_count, result.error);
                        }
                    });
                }

                for (auto& worker : workers) {
                    worker.join();
                }

                for (size_t i = 0; i < missing_event_slugs.size(); ++i) {
                    const std::string& event_slug = missing_event_slugs[i];
                    const auto& result = results[i];
                    if (!result.ok) {
                        ++event_failures;
                        std::fprintf(stderr, "[GROUP] %s | event lookup failed: %s\n",
                                     event_slug.c_str(), result.error.c_str());
                        continue;
                    }

                    event_market_counts.emplace(event_slug, result.market_count);
                    metadata_cache.event_market_counts[event_slug] = result.market_count;
                    ++event_fetched;
                }
            }

            contract_groups.reserve(contract_groups.size() + contracts_by_event.size());
            for (const auto& [event_slug, members] : contracts_by_event) {
                if (members.size() < 2) continue;
                const auto count_it = event_market_counts.find(event_slug);
                if (count_it == event_market_counts.end() || count_it->second != members.size()) {
                    continue;
                }

                ContractGroup group;
                group.key = event_slug;
                group.display_name = members.front()->event_title.empty()
                    ? event_slug
                    : members.front()->event_title;
                group.exhaustive = true;
                group.contracts = members;
                if (!seen_group_keys.emplace(group.key).second) {
                    continue;
                }
                contract_groups.push_back(std::move(group));
            }

            const auto group_phase_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - group_phase_start).count();
            std::printf("[GROUP] Auto-detect | candidates=%zu cache_hits=%zu fetched=%zu failed=%zu groups=%zu elapsed=%.2fms\n",
                        contracts_by_event.size(),
                        event_cache_hits,
                        event_fetched,
                        event_failures,
                        contract_groups.size(),
                        group_phase_ms);
        }

        if (!config.market_metadata_cache_file.empty()) {
            std::string cache_error;
            if (!save_market_metadata_cache(config.market_metadata_cache_file, metadata_cache, cache_error)) {
                std::fprintf(stderr, "[META] cache save failed: %s\n", cache_error.c_str());
            }
        }
    }

    if (config.enable_group_arbitrage) {
        for (auto& group : contract_groups) {
            std::printf("[GROUP] Enabled exhaustive basket: %s | legs=%zu\n",
                        group.display_name.c_str(), group.contracts.size());
            for (Contract* contract : group.contracts) {
                contract_group_routes[contract].push_back(&group);
            }
        }
    }

    // Build token_id → Contract* lookup map
    std::unordered_map<std::string_view, TokenRoute,
                       std::hash<std::string_view>, std::equal_to<>> token_routes;
    token_routes.reserve(config.contracts.size() * 2);
    token_routes.max_load_factor(0.7f);

    for (auto& c : config.contracts) {
        token_routes.emplace(c.token_id_yes, TokenRoute{&c, true});
        token_routes.emplace(c.token_id_no, TokenRoute{&c, false});
        printf("[INIT] Contract: %s\n", c.asset_name.c_str());
        printf("       YES token: %.40s...\n", c.token_id_yes.c_str());
        printf("       NO  token: %.40s...\n", c.token_id_no.c_str());
    }

    auto message_queue = std::make_unique<SpscRing<MessageSlot>>(config.message_queue_capacity);
    auto metrics_queue = std::make_unique<SpscRing<MetricsEvent>>(config.metrics_queue_capacity);
    auto opportunity_queue = std::make_unique<SpscRing<ArbOpportunity>>(config.opportunity_queue_capacity);
    auto replay_queue = std::make_unique<SpscRing<ReplaySnapshot>>(config.replay_queue_capacity);
    // OMS(parser thread) -> cancel-sender thread. Cancels/creates leave the hot
    // path here so ACR detection never blocks on the gateway's I/O.
    auto command_queue = std::make_unique<SpscRing<OrderCommand>>(config.command_queue_capacity);
    // Quote telemetry (parser thread -> logger thread); off the hot timing path.
    auto quote_telemetry_queue =
        std::make_unique<SpscRing<QuoteTelemetryEvent>>(config.quote_telemetry_queue_capacity);
    std::atomic<uint64_t> dropped_quote_telemetry{0};
    // Reward-refresh thread -> parser thread (markets going rates:null mid-session).
    auto reward_update_queue =
        std::make_unique<SpscRing<RewardConfigUpdate>>(config.reward_update_queue_capacity);
    // Cancel-sender telemetry (written only by the sender thread; read after join).
    std::atomic<bool> sender_stop{false};
    LatencyTracker sender_send_us;
    uint64_t sender_exec_creates = 0, sender_exec_cancels = 0;

    std::atomic<bool> receiver_done{false};
    std::atomic<uint64_t> dropped_oversize_messages{0};
    std::atomic<uint64_t> dropped_metrics_events{0};
    std::atomic<uint64_t> dropped_opportunities{0};
    std::atomic<uint64_t> dropped_replay_snapshots{0};
    std::atomic<uint64_t> message_queue_backpressure_events{0};
    std::atomic<uint64_t> message_queue_backpressure_spins{0};
    std::atomic<uint64_t> max_message_bytes_seen{0};
    std::atomic<uint64_t> max_frame_opportunities_seen{0};
    WebSocketClientStats websocket_stats{};

    auto logger = std::make_unique<Logger>(config);

    auto push_metrics = [&](const MetricsEvent& event) {
        if (event.type == MetricsEventType::EDGE_SAMPLE) {
            if (!config.edge_telemetry_enabled) return;
        } else if (!config.metrics_enabled) {
            return;
        }
        if (!metrics_queue->try_push_copy(event)) {
            dropped_metrics_events.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_opportunity = [&](const ArbOpportunity& opp) {
        if (!opportunity_queue->try_push_copy(opp)) {
            dropped_opportunities.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_replay_snapshot = [&](const ReplaySnapshot& snapshot) {
        if (!config.replay_logging_enabled) return;
        if (!replay_queue->try_push_copy(snapshot)) {
            dropped_replay_snapshots.fetch_add(1, std::memory_order_relaxed);
        }
    };

    auto push_quote_telemetry = [&](const QuoteTelemetryEvent& qt) {
        if (!quote_telemetry_queue->try_push_copy(qt)) {
            dropped_quote_telemetry.fetch_add(1, std::memory_order_relaxed);
        }
    };

    std::thread logger_thread([&] {
        apply_thread_runtime_tuning(
            "logger",
            config.logger_cpu,
            config.logger_priority > 0 ? config.logger_priority : config.realtime_priority,
            config.prefault_stack_kb);

        // Quote-telemetry CSV is owned by (and written only on) this thread.
        std::ofstream qt_file;
        if (config.quote_telemetry_enabled) {
            qt_file.open(config.quote_telemetry_log_file, std::ios::out);
            if (qt_file) {
                qt_file << "sample_ns,token,mid_thou,spread_thou,bid_px,ask_px,bid_dist_thou,"
                           "ask_dist_thou,est_qmin,net_position,vol_thou,bid_at_risk,ask_at_risk,"
                           "off_grid,too_wide,eligible,neg_risk\n";
            }
        }
        auto drain_quote_telemetry = [&]() -> bool {
            bool did = false;
            QuoteTelemetryEvent* qt = nullptr;
            while (quote_telemetry_queue->front(qt)) {
                did = true;
                if (qt_file) {
                    qt_file << qt->sample_ns << ',' << qt->token << ',' << qt->mid_thou << ','
                            << qt->spread_thou << ',' << qt->bid_px << ',' << qt->ask_px << ','
                            << qt->bid_dist_thou << ',' << qt->ask_dist_thou << ','
                            << qt->est_qmin << ',' << qt->net_position << ',' << qt->vol_thou << ','
                            << (qt->bid_at_risk ? 1 : 0) << ',' << (qt->ask_at_risk ? 1 : 0) << ','
                            << (qt->off_grid ? 1 : 0) << ',' << (qt->too_wide ? 1 : 0) << ','
                            << (qt->eligible ? 1 : 0) << ',' << (qt->neg_risk ? 1 : 0) << '\n';
                }
                quote_telemetry_queue->pop();
            }
            return did;
        };

        while (g_running || !receiver_done.load() || !metrics_queue->empty() ||
               !opportunity_queue->empty() || !replay_queue->empty() ||
               !quote_telemetry_queue->empty()) {
            bool did_work = false;

            MetricsEvent* metric = nullptr;
            while (metrics_queue->front(metric)) {
                did_work = true;
                if (metric->has_feed_delivery) {
                    logger->consume_feed_delivery(metric->feed_delivery_ms);
                }
                switch (metric->type) {
                    case MetricsEventType::MESSAGE:
                        logger->consume_message(metric->msg_bytes);
                        break;
                    case MetricsEventType::BOOK:
                        logger->consume_book(metric->event_count, metric->has_latency_sample,
                                             metric->queue_us, metric->parse_us, metric->book_us,
                                             metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::PRICE_CHANGE:
                        logger->consume_price_change(metric->event_count, metric->has_latency_sample,
                                                     metric->queue_us, metric->parse_us, metric->book_us,
                                                     metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::BEST_BID_ASK:
                        logger->consume_bbo(metric->event_count, metric->has_latency_sample,
                                            metric->queue_us, metric->parse_us, metric->book_us,
                                            metric->arb_us, metric->e2e_us, metric->arb_checks);
                        break;
                    case MetricsEventType::TRADE:
                        logger->consume_trade(metric->event_count);
                        break;
                    case MetricsEventType::EDGE_SAMPLE:
                        logger->consume_edge_sample(metric->sample_time_ns,
                                                    metric->edge_label,
                                                    metric->arb_kind,
                                                    metric->reference_value,
                                                    metric->leg_count,
                                                    metric->raw_edge_bps,
                                                    metric->net_edge_bps);
                        break;
                }
                metrics_queue->pop();
            }

            ArbOpportunity* opp = nullptr;
            while (opportunity_queue->front(opp)) {
                did_work = true;
                logger->consume_opportunity(*opp);
                opportunity_queue->pop();
            }

            ReplaySnapshot* snapshot = nullptr;
            while (replay_queue->front(snapshot)) {
                did_work = true;
                logger->consume_replay_snapshot(*snapshot);
                replay_queue->pop();
            }

            did_work |= drain_quote_telemetry();

            logger->flush_pending_csv();
            logger->maybe_print_summary();

            if (!did_work) {
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
        }

        std::printf("\n[FINAL]\n");
        logger->print_final_summary();
    });

    std::thread receiver_thread([&] {
        apply_thread_runtime_tuning(
            "receiver",
            config.receiver_cpu,
            config.receiver_priority > 0 ? config.receiver_priority : config.realtime_priority,
            config.prefault_stack_kb);

        WebSocketClient ws(config);

        std::printf("\n[STARTING] Connecting to %s...\n\n", config.websocket_host.c_str());
        if (!ws.connect()) {
            std::fprintf(stderr, "[FATAL] Could not connect to WebSocket\n");
            websocket_stats = ws.stats();
            g_running = false;
            receiver_done = true;
            return;
        }

        ws.run([&](const char* data, size_t len, NanoTime t0_recv, NanoTime t0_recv_wall) {
            update_atomic_max(max_message_bytes_seen, static_cast<uint64_t>(len));
            if (len > MessageParser::kBufferCapacity) {
                dropped_oversize_messages.fetch_add(1, std::memory_order_relaxed);
                if (config.hot_path_logging) {
                    std::fprintf(stderr, "[DROP] Oversize message: %zu bytes\n", len);
                }
                return;
            }

            bool waited_for_slot = false;
            while (g_running &&
                   !message_queue->try_push([&](MessageSlot& slot) {
                       store_message_slot(slot, data, len, t0_recv, t0_recv_wall);
                   })) {
                waited_for_slot = true;
                message_queue_backpressure_spins.fetch_add(1, std::memory_order_relaxed);
                cpu_relax();
            }
            if (waited_for_slot) {
                message_queue_backpressure_events.fetch_add(1, std::memory_order_relaxed);
            }
        }, [&] {
            return !g_running.load(std::memory_order_relaxed);
        });

        websocket_stats = ws.stats();
        receiver_done = true;
    });

    // Execution mode selects the send-side gateway: Shadow logs; MockLive builds
    // the real v2 order + EIP-712 digest (no key/sign/send); Live is gated/unbuilt.
    // Built here in main scope so the sender thread uses it and the summary reads
    // its stats after join. Everything upstream (OMS/reconcile/risk/ACR) is
    // mode-agnostic — it only ever sees the IExecGateway* below.
    ExecMode exec_mode = ExecMode::Shadow;
    if (config.exec_mode == "mocklive") exec_mode = ExecMode::MockLive;
    else if (config.exec_mode == "live") exec_mode = ExecMode::Live;

    ShadowGateway shadow_exec(config.shadow_executor_verbose);
    live::SignerConfig signer_cfg;
    signer_cfg.maker = config.live_maker_address;
    signer_cfg.signer = config.live_signer_address;
    signer_cfg.signature_type = static_cast<live::SigType>(config.live_signature_type);
    live::MockLiveGateway mock_exec(signer_cfg, config.shadow_executor_verbose);

    IExecGateway* exec_ptr = &shadow_exec;
    if (config.shadow_executor_enabled) {
        if (exec_mode == ExecMode::MockLive) {
            exec_ptr = &mock_exec;
            live::MockLiveGateway::describe();
        } else if (exec_mode == ExecMode::Live) {
            std::printf("\n[LIVE] *** execution path is NOT built and is GATED "
                        "(compliance/custody/v2 signing). Running SHADOW — no orders sent. ***\n\n");
            // exec_ptr stays &shadow_exec: nothing is ever signed or sent.
        }
        std::printf("[EXEC] mode=%s\n", exec_mode_name(exec_mode));
    }

    // Cancel-sender thread: drains the OMS command ring and executes the actual
    // create/cancel via the selected gateway. Pinned, off the hot path, so ACR
    // detection on the parser thread never blocks on the gateway's work.
    std::thread sender_thread([&] {
        if (!config.shadow_executor_enabled) { return; }
        apply_thread_runtime_tuning(
            "sender",
            config.sender_cpu,
            config.sender_priority > 0 ? config.sender_priority : config.realtime_priority,
            config.prefault_stack_kb);
        IExecGateway& exec = *exec_ptr;
        while (!sender_stop.load(std::memory_order_acquire) || !command_queue->empty()) {
            OrderCommand* cmd = nullptr;
            if (command_queue->front(cmd)) {
                const NanoTime t_exec = now_ns();
                if (cmd->kind == OrderCommand::Kind::CREATE) { exec.submit(cmd->order); ++sender_exec_creates; }
                else                                          { exec.cancel(cmd->order); ++sender_exec_cancels; }
                if (cmd->decided_ns) {
                    sender_send_us.record(static_cast<double>(t_exec - cmd->decided_ns) / 1000.0);
                }
                command_queue->pop();
            } else {
                cpu_relax();
            }
        }
    });

    // Reward-config refresh thread: markets rotate (rates:null) mid-session; re-fetch
    // each market every reward_refresh_seconds and hand updates to the parser thread
    // via an SPSC ring (parser is the sole writer of Contract reward fields — no lock
    // on the hot read path). Off by default (reward_refresh_seconds=0).
    std::thread reward_refresh_thread([&] {
        if (!config.shadow_executor_enabled || config.reward_refresh_seconds <= 0) return;
        apply_thread_runtime_tuning("reward-refresh", -1, 0, config.prefault_stack_kb);
        ClobRewardsClient client;
        while (g_running) {
            for (int s = 0; s < config.reward_refresh_seconds && g_running; ++s)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            if (!g_running) break;
            for (size_t i = 0; i < config.contracts.size(); ++i) {
                if (config.contracts[i].condition_id.empty()) continue;
                RewardConfigRaw rc;
                std::string err;
                if (!client.fetch(config.contracts[i].condition_id, rc, err)) continue;  // keep last
                RewardConfigUpdate u{static_cast<uint32_t>(i), rc.active,
                                     static_cast<uint16_t>(rc.max_spread_thou),
                                     static_cast<uint32_t>(rc.min_size), rc.daily_rate_usd};
                reward_update_queue->try_push_copy(u);  // drop if full; re-sent next cycle
            }
        }
    });

    apply_thread_runtime_tuning(
        "parser",
        config.parser_cpu >= 0 ? config.parser_cpu : config.pin_thread_cpu,
        config.parser_priority > 0 ? config.parser_priority : config.realtime_priority,
        config.prefault_stack_kb);

    auto parser = std::make_unique<MessageParser>();
    ArbitrageDetector arb(config);
    uint64_t next_frame_id = 1;
    std::vector<ArbOpportunity> frame_opportunities;
    frame_opportunities.reserve(
        std::max<size_t>(ArbCheckOutput::kMaxOpportunities,
                         config.contracts.size() * ArbCheckOutput::kMaxOpportunities));
    std::vector<Contract*> touched_contracts;
    touched_contracts.reserve(config.contracts.size());
    std::vector<ContractGroup*> touched_groups;
    touched_groups.reserve(contract_groups.size());
    std::vector<Orderbook*> touched_books;
    touched_books.reserve(config.contracts.size() * 2);

    // Shadow liquidity-rewards executor: owns its order state on THIS (parser)
    // thread. Reconciled AFTER the arb timing stamp so it never contaminates
    // arb_us/e2e. Its gateway only ENQUEUES to the sender thread (no I/O here).
    ThreadedGateway shadow_gateway(
        [&](const OrderCommand& c) { return command_queue->try_push_copy(c); });
    RiskLimits shadow_limits;
    shadow_limits.max_gross_notional_usd = config.risk_max_gross_notional_usd;
    shadow_limits.max_position_shares = config.risk_max_position_shares;
    shadow_limits.max_open_orders_total = config.risk_max_open_orders_total;
    shadow_limits.max_collateral_usd = config.risk_pusd_allowance_usd;  // pUSD allowance sim
    Oms shadow_oms(shadow_gateway, shadow_limits);

    // Startup reconciliation seam: a LiveGateway rebuilds OMS state from the
    // account's open orders so a restart/crash doesn't orphan resting orders.
    // No-op for shadow/mock (no exchange state) — this is the live plug-in point.
    if (config.shadow_executor_enabled) {
        std::vector<ManagedOrder> resting;
        if (exec_ptr->adopt_open_orders(resting)) {
            std::printf("[OMS] startup reconciliation: %zu resting order(s) to adopt\n",
                        resting.size());
            // (A LiveGateway feeds `resting` into shadow_oms here.)
        }
    }
    double shadow_qmin_sum = 0.0;     // Σ est_qmin over reconciles (telemetry)
    uint64_t shadow_qmin_count = 0;
    double shadow_daily_pool_sum = 0.0;  // Σ daily pool over eligible markets seen

    // ACR (anti-cancel-race): inline detection on this thread; cancels go out via
    // the sender thread. acr_react_us measures trigger(book recv) -> at-risk detect.
    AcrConfig acr_cfg;
    acr_cfg.enabled = config.acr_enabled;
    acr_cfg.stale_drift_ticks = config.acr_stale_drift_ticks;
    acr_cfg.inv_skew_per_share_thou = config.acr_inv_skew_per_share_thou;
    acr_cfg.vol_widen_k = config.acr_vol_widen_k;
    LatencyTracker acr_react_us;   // parser-thread local; read after the loop
    uint64_t acr_events = 0;       // book updates where a resting quote was at-risk

    // Adaptive quote throttle (suppress re-pricing churn; ACR/side-change bypass).
    ThrottleConfig thr_cfg;
    thr_cfg.enabled = config.quote_throttle_enabled;
    thr_cfg.min_interval_ms = config.quote_throttle_min_ms;
    thr_cfg.max_interval_ms = config.quote_throttle_max_ms;
    thr_cfg.vol_hot_thou = static_cast<float>(config.quote_throttle_vol_hot_thou);
    uint64_t throttle_skips = 0;   // reconciles suppressed by the throttle

    // Pre-send quote audit -> near_miss_live.csv (mocklive only; rare writes,
    // single-writer on this parser thread, off the hot timing path).
    std::unique_ptr<live::NearMissLiveLog> near_miss_live;
    if (config.shadow_executor_enabled && exec_mode == ExecMode::MockLive) {
        near_miss_live = std::make_unique<live::NearMissLiveLog>(config.near_miss_live_log_file);
    }

    auto collect_output = [&](const ArbCheckOutput& output, NanoTime t0_recv,
                              NanoTime t1_parse_done, NanoTime t2_books_done) -> uint16_t {
        for (size_t i = 0; i < output.edge_sample_count; ++i) {
            const EdgeTelemetrySample& sample = output.edge_samples[i];
            if (!sample.valid) continue;

            MetricsEvent edge_event{};
            edge_event.type = MetricsEventType::EDGE_SAMPLE;
            edge_event.sample_time_ns = sample.sample_time_ns;
            edge_event.edge_label = sample.label;
            edge_event.arb_kind = sample.arb_kind;
            edge_event.reference_value = sample.reference_value;
            edge_event.leg_count = sample.leg_count;
            edge_event.raw_edge_bps = sample.raw_edge_bps;
            edge_event.net_edge_bps = sample.net_edge_bps;
            push_metrics(edge_event);
        }

        for (size_t i = 0; i < output.opportunity_count; ++i) {
            auto log_opp = output.opportunities[i];
            log_opp.t0_ws_recv_ns = t0_recv;
            log_opp.t1_parse_done_ns = config.metrics_enabled ? t1_parse_done : t0_recv;
            log_opp.t2_book_updated_ns = config.metrics_enabled ? t2_books_done : t0_recv;
            log_opp.t3_arb_checked_ns = config.metrics_enabled ? log_opp.t3_arb_checked_ns : t0_recv;
            log_opp.t4_logged_ns = config.metrics_enabled ? now_ns() : t0_recv;
            log_opp.paper_trade_pnl = 0.0;
            log_opp.counts_toward_paper_trade = false;
            frame_opportunities.push_back(log_opp);
        }

        return output.checks_performed;
    };

    // Dead-man's-switch state (parser-thread local).
    const uint64_t dms_threshold_ns = config.dead_mans_switch_seconds > 0
        ? static_cast<uint64_t>(config.dead_mans_switch_seconds) * 1000000000ull : 0;
    uint64_t last_msg_ns = now_ns();
    bool dms_fired = false;
    uint64_t dms_trips = 0;

    while (g_running || !receiver_done.load() || !message_queue->empty()) {
        // Apply any reward-config refreshes (this thread is the sole writer of the
        // Contract reward fields the executor reads below — no lock needed).
        {
            RewardConfigUpdate* ru = nullptr;
            while (reward_update_queue->front(ru)) {
                if (ru->contract_index < config.contracts.size()) {
                    Contract& rc = config.contracts[ru->contract_index];
                    const bool was = rc.reward_active;
                    rc.reward_active = ru->active;
                    rc.reward_max_spread_thou = ru->max_spread_thou;
                    rc.reward_min_size = ru->min_size;
                    rc.reward_daily_rate_usd = ru->daily_rate_usd;
                    if (was != ru->active)
                        std::fprintf(stderr, "[REWARDS] %s now %s\n", rc.asset_name.c_str(),
                                     ru->active ? "ACTIVE" : "inactive (rates:null)");
                }
                reward_update_queue->pop();
            }
        }

        MessageSlot* slot = nullptr;
        if (!message_queue->front(slot)) {
            if (receiver_done.load()) {
                break;
            }
            // Dead-man's-switch: feed stale while running → flatten (cancel every
            // resting order) + halt new ones. A disconnect leaves orders
            // un-managed at the exchange (no data to react to) = unbounded adverse
            // selection. Re-arms (resumes quoting) when data returns below.
            if (dms_threshold_ns && g_running && config.shadow_executor_enabled &&
                !dms_fired && (now_ns() - last_msg_ns) > dms_threshold_ns) {
                shadow_oms.cancel_everything();
                shadow_oms.set_kill_switch(true);
                dms_fired = true;
                ++dms_trips;
                std::fprintf(stderr, "[DMS] feed stale >%ds — cancelled all, halted new orders\n",
                             config.dead_mans_switch_seconds);
            }
            cpu_relax();
            continue;
        }

        last_msg_ns = now_ns();
        if (dms_fired) {                       // feed resumed → re-enable quoting
            shadow_oms.set_kill_switch(false);
            dms_fired = false;
            std::fprintf(stderr, "[DMS] feed resumed — quoting re-enabled\n");
        }

        if (config.metrics_enabled) {
            MetricsEvent msg_event{};
            msg_event.type = MetricsEventType::MESSAGE;
            msg_event.msg_bytes = static_cast<uint32_t>(slot->len);
            push_metrics(msg_event);
        }

        struct FrameStats {
            uint16_t book_events = 0;
            uint16_t price_change_events = 0;
            uint16_t bbo_events = 0;
            uint16_t trade_events = 0;
            uint64_t book_apply_ns = 0;
            uint64_t max_event_ts = 0;   // newest exchange timestamp (ms) seen this frame
        } frame{};
        frame_opportunities.clear();
        touched_contracts.clear();
        touched_groups.clear();
        touched_books.clear();

        const uint64_t frame_id = next_frame_id++;
        const NanoTime t1_parse_start = config.metrics_enabled ? now_ns() : 0;
        const float queue_us = config.metrics_enabled
            ? static_cast<float>((t1_parse_start - slot->recv_time) / 1000.0)
            : 0.0f;

        auto mark_contract_touched = [&](Contract& contract) {
            if (contract.touched_frame_id != frame_id) {
                contract.touched_frame_id = frame_id;
                touched_contracts.push_back(&contract);
            }

            if (!config.enable_group_arbitrage) return;
            const auto group_it = contract_group_routes.find(&contract);
            if (group_it == contract_group_routes.end()) return;
            for (ContractGroup* group : group_it->second) {
                if (group->touched_frame_id != frame_id) {
                    group->touched_frame_id = frame_id;
                    touched_groups.push_back(group);
                }
            }
        };

        auto mark_book_touched = [&](Orderbook& book) {
            if (book.touched_frame_id != frame_id) {
                book.touched_frame_id = frame_id;
                touched_books.push_back(&book);
            }
        };

        auto apply_update = [&](Contract& contract, Orderbook& book, uint64_t timestamp, auto&& update_fn) {
            const NanoTime t_book_start = config.metrics_enabled ? now_ns() : 0;
            update_fn(book);
            book.timestamp = timestamp;
            if (timestamp > frame.max_event_ts) frame.max_event_ts = timestamp;
            ++contract.total_updates;
            if (&book == &contract.book_yes) {
                ++contract.yes_updates;
            } else {
                ++contract.no_updates;
            }
            if (config.metrics_enabled) {
                frame.book_apply_ns += now_ns() - t_book_start;
            }
            mark_book_touched(book);
            mark_contract_touched(contract);
        };

        parser->parse_padded(slot->data, slot->len,
            MessageParser::kBufferCapacity + simdjson::SIMDJSON_PADDING,
            [&](const ParsedBookEvent& ev, simdjson::ondemand::object& obj) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    std::printf("[BOOK] Unknown asset_id: %.*s...\n",
                                (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    return;
                }

                ++frame.book_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::full_update(target, obj);
                });
                // A book snapshot is the authoritative resync point: adopt its
                // tick (when present) and record its hash. NOTE: full byte-level
                // gap detection (recompute local book hash, compare to ev.hash,
                // REST-resnapshot on mismatch) needs Polymarket's exact book
                // hashing scheme, which the WS feed does not document. Until then
                // the resync path is reconnect-driven: any disconnect/oversize
                // drop forces a fresh initial_dump snapshot of every book.
                if (ev.tick_size > 0) book.tick_thou = ev.tick_size;
                if (ev.hash != 0) book.last_hash = ev.hash;
            },
            [&](const ParsedTradeEvent& ev) {
                ++frame.trade_events;
                if (config.hot_path_logging) {
                    auto it = token_routes.find(ev.asset_id);
                    std::string label = "???";
                    if (it != token_routes.end()) {
                        label = it->second.contract->asset_name + (it->second.is_yes ? " YES" : " NO");
                    }

                    std::printf("[TRADE] %-25s | price=%.*s side=%.*s size=%.*s\n",
                                label.c_str(),
                                (int)ev.price_str.size(), ev.price_str.data(),
                                (int)ev.side.size(), ev.side.data(),
                                (int)ev.size_str.size(), ev.size_str.data());
                }
            },
            [&](const ParsedPriceChangeEvent& ev) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[PXCHG] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                ++frame.price_change_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::apply_price_change(target, ev);
                });
                if (ev.hash != 0) book.last_hash = ev.hash;
            },
            [&](const ParsedBestBidAskEvent& ev) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) {
                    if (config.hot_path_logging) {
                        std::printf("[BBO] Unknown asset_id: %.*s...\n",
                                    (int)std::min<size_t>(ev.asset_id.size(), 40), ev.asset_id.data());
                    }
                    return;
                }

                ++frame.bbo_events;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                apply_update(contract, book, ev.timestamp, [&](Orderbook& target) {
                    OrderbookManager::apply_best_bid_ask(target, ev);
                });
            },
            [&](const ParsedTickSizeEvent& ev) {
                auto it = token_routes.find(ev.asset_id);
                if (it == token_routes.end()) return;
                Contract& contract = *it->second.contract;
                Orderbook& book = it->second.is_yes ? contract.book_yes : contract.book_no;
                if (ev.tick_size > 0) book.tick_thou = ev.tick_size;
            });

        const NanoTime t2_parse_done = config.metrics_enabled ? now_ns() : 0;
        const NanoTime t3_books_done = config.metrics_enabled ? now_ns() : 0;

        for (Orderbook* book : touched_books) {
            book->local_update_ns = t3_books_done;
        }

        uint16_t arb_checks = 0;
        for (Contract* contract : touched_contracts) {
            if (!contract_ready_for_arb(*contract)) {
                continue;
            }

            if (config.hot_path_logging) {
                std::printf("[CONTRACT] %s | YES: %u/%u | NO: %u/%u | sum_asks=%u sum_bids=%u\n",
                            contract->asset_name.c_str(),
                            contract->book_yes.best_bid, contract->book_yes.best_ask,
                            contract->book_no.best_bid, contract->book_no.best_ask,
                            static_cast<uint16_t>(contract->book_yes.best_ask + contract->book_no.best_ask),
                            static_cast<uint16_t>(contract->book_yes.best_bid + contract->book_no.best_bid));
            }

            arb_checks = static_cast<uint16_t>(
                arb_checks + collect_output(arb.check(*contract, t3_books_done),
                                            slot->recv_time, t2_parse_done, t3_books_done));
        }

        if (config.enable_group_arbitrage) {
            for (ContractGroup* group : touched_groups) {
                if (!group_ready_for_arb(*group)) {
                    continue;
                }
                arb_checks = static_cast<uint16_t>(
                    arb_checks + collect_output(arb.check_group(*group, t3_books_done),
                                                slot->recv_time, t2_parse_done, t3_books_done));
            }
        }

        // Stamp arb-done HERE, before paper-trade selection / opportunity push /
        // replay emission. Those are output/logging work; folding them into
        // arb_us and e2e_us (as the old placement after the replay loop did)
        // inflated both whenever replay logging was on, contaminating run-to-run
        // latency comparisons.
        const NanoTime t4_arb_done = config.metrics_enabled ? now_ns() : 0;

        // ---- Shadow liquidity-rewards executor + ACR (post-arb, off timing path) ----
        // Per touched reward-active market, per token (YES/NO are independent reward
        // markets): (1) ACR updates vol + checks if a resting quote is now at-risk
        // (cancel race), (2) RewardQuoter computes the base quote, (3) ACR applies
        // inventory skew + vol widening, (4) OMS reconciles (cancels via sender thr).
        if (config.shadow_executor_enabled) {
            auto quote_token = [&](const std::string& tok, Orderbook& book,
                                   const RewardConfig& rcfg, const RewardQuoteParams& params,
                                   bool neg_risk) {
                AcrEngine::update_vol(book);
                const uint32_t mid2 = midpoint2_thou(book);
                const LiveSide live = shadow_oms.live_side(tok);
                const AcrRisk risk = AcrEngine::assess(book, live, acr_cfg);
                if (risk.any()) {
                    ++acr_events;   // a resting quote is at-risk -> ACR cancels it now
                    acr_react_us.record(static_cast<double>(now_ns() - slot->recv_time) / 1000.0);
                }
                DesiredQuotes base = RewardQuoter::quote(book, rcfg, params);
                const double net_pos = shadow_oms.net_position(tok);
                DesiredQuotes desired = AcrEngine::adjust(base, book, net_pos, acr_cfg);

                // Throttle: hold the resting quote between moves to preserve queue
                // priority / spare rate limits; but NEVER block an action that must
                // go out now — ACR at-risk, or a side being added/removed.
                const bool side_change = (desired.bid.valid != live.bid.has) ||
                                         (desired.ask.valid != live.ask.has);
                const bool must_act = risk.any() || side_change;
                const bool allowed = QuoteThrottle::allow(book, thr_cfg, now_ns(), must_act);

                // Pre-send audit (mocklive): off-grid / too-wide / self-cross.
                live::QuoteIssues issues{};
                if (near_miss_live) {
                    issues = live::validate_quote(desired, book.tick_thou,
                                                  rcfg.max_spread_thou, mid2);
                    near_miss_live->record(tok, mid2, book.tick_thou, desired, issues);
                }

                if (allowed) shadow_oms.reconcile(tok, desired, mid2, neg_risk);
                else         ++throttle_skips;

                if (base.eligible) { shadow_qmin_sum += base.est_qmin; ++shadow_qmin_count; }

                // Quote telemetry (Roadmap #1) -> ring -> logger thread CSV.
                if (config.quote_telemetry_enabled) {
                    const int mid_thou = static_cast<int>(mid2 / 2);
                    QuoteTelemetryEvent qt{};
                    qt.sample_ns = now_realtime_ns();
                    qt.token = tok;  // string_view into Contract token id (process-stable)
                    qt.mid_thou = static_cast<uint16_t>(mid_thou);
                    qt.spread_thou = (book.best_ask > book.best_bid)
                        ? static_cast<uint16_t>(book.best_ask - book.best_bid) : 0;
                    qt.bid_px = desired.bid.valid ? desired.bid.price : 0;
                    qt.ask_px = desired.ask.valid ? desired.ask.price : 0;
                    qt.bid_dist_thou = desired.bid.valid
                        ? static_cast<int16_t>(static_cast<int>(desired.bid.price) - mid_thou) : 0;
                    qt.ask_dist_thou = desired.ask.valid
                        ? static_cast<int16_t>(static_cast<int>(desired.ask.price) - mid_thou) : 0;
                    qt.est_qmin = static_cast<float>(base.est_qmin);
                    qt.net_position = static_cast<float>(net_pos);
                    qt.vol_thou = book.acr_vol_thou;
                    qt.bid_at_risk = risk.bid_at_risk;
                    qt.ask_at_risk = risk.ask_at_risk;
                    qt.off_grid = issues.off_grid;
                    qt.too_wide = issues.too_wide;
                    qt.eligible = base.eligible;
                    qt.neg_risk = neg_risk;
                    push_quote_telemetry(qt);
                }
            };
            for (Contract* contract : touched_contracts) {
                if (!contract->reward_active) continue;
                RewardConfig rcfg;
                rcfg.active = true;
                rcfg.max_spread_thou = contract->reward_max_spread_thou;
                rcfg.min_size = contract->reward_min_size;
                rcfg.daily_rate_usd = contract->reward_daily_rate_usd;

                RewardQuoteParams params;
                params.quote_size = config.reward_quote_size > 0
                    ? config.reward_quote_size : rcfg.min_size;
                params.target_offset_thou = static_cast<Price>(config.reward_target_offset_thou);

                quote_token(contract->token_id_yes, contract->book_yes, rcfg, params, contract->neg_risk);
                quote_token(contract->token_id_no, contract->book_no, rcfg, params, contract->neg_risk);
                shadow_daily_pool_sum += rcfg.daily_rate_usd;
            }
        }

        size_t best_paper_trade_index = frame_opportunities.size();
        auto is_better_paper_trade = [](const ArbOpportunity& lhs, const ArbOpportunity& rhs) {
            if (lhs.theoretical_pnl != rhs.theoretical_pnl) {
                return lhs.theoretical_pnl < rhs.theoretical_pnl;
            }
            if (lhs.edge_bps != rhs.edge_bps) {
                return lhs.edge_bps < rhs.edge_bps;
            }
            if (lhs.size_shares != rhs.size_shares) {
                return lhs.size_shares < rhs.size_shares;
            }
            return lhs.leg_count < rhs.leg_count;
        };

        for (size_t i = 0; i < frame_opportunities.size(); ++i) {
            const ArbOpportunity& opp = frame_opportunities[i];
            if (is_maker_arb_kind(opp.arb_kind)) {
                continue;
            }
            if (opp.theoretical_pnl <= 0.0) {
                continue;
            }
            if (best_paper_trade_index == frame_opportunities.size() ||
                is_better_paper_trade(frame_opportunities[best_paper_trade_index], opp)) {
                best_paper_trade_index = i;
            }
        }

        if (best_paper_trade_index < frame_opportunities.size()) {
            ArbOpportunity& best_opp = frame_opportunities[best_paper_trade_index];
            best_opp.counts_toward_paper_trade = true;
            best_opp.paper_trade_pnl = best_opp.theoretical_pnl;
        }
        update_atomic_max(max_frame_opportunities_seen,
                          static_cast<uint64_t>(frame_opportunities.size()));

        for (const ArbOpportunity& opp : frame_opportunities) {
            push_opportunity(opp);
        }

        if (config.replay_logging_enabled) {
            auto was_touched = [&](const Contract* contract) {
                return contract->touched_frame_id == frame_id;
            };

            auto emit_replay = [&](std::string_view event_key, std::string_view event_label,
                                   const Contract& contract, uint8_t leg_index, uint8_t leg_count) {
                ReplaySnapshot snapshot{};
                snapshot.frame_id = frame_id;
                snapshot.t0_ws_recv_ns = slot->recv_time;
                snapshot.t1_parse_start_ns = t1_parse_start;
                snapshot.t2_parse_done_ns = t2_parse_done;
                snapshot.t3_books_done_ns = t3_books_done;
                snapshot.frame_bytes = static_cast<uint32_t>(slot->len);
                snapshot.book_events = frame.book_events;
                snapshot.price_change_events = frame.price_change_events;
                snapshot.bbo_events = frame.bbo_events;
                snapshot.trade_events = frame.trade_events;
                snapshot.event_key = event_key;
                snapshot.event_label = event_label;
                snapshot.contract_name = contract.asset_name;
                snapshot.leg_index = leg_index;
                snapshot.leg_count = leg_count;
                snapshot.touched_in_frame = was_touched(&contract);
                snapshot.yes_exchange_ts = contract.book_yes.timestamp;
                snapshot.no_exchange_ts = contract.book_no.timestamp;
                snapshot.yes_bid = contract.book_yes.best_bid;
                snapshot.yes_bid_size = contract.book_yes.best_bid_size;
                snapshot.yes_ask = contract.book_yes.best_ask;
                snapshot.yes_ask_size = contract.book_yes.best_ask_size;
                snapshot.no_bid = contract.book_no.best_bid;
                snapshot.no_bid_size = contract.book_no.best_bid_size;
                snapshot.no_ask = contract.book_no.best_ask;
                snapshot.no_ask_size = contract.book_no.best_ask_size;
                push_replay_snapshot(snapshot);
            };

            for (const ContractGroup* group : touched_groups) {
                for (size_t i = 0; i < group->contracts.size(); ++i) {
                    emit_replay(group->key, group->display_name, *group->contracts[i],
                                static_cast<uint8_t>(i), static_cast<uint8_t>(group->contracts.size()));
                }
            }

            for (Contract* contract : touched_contracts) {
                const auto group_it = contract_group_routes.find(contract);
                if (group_it != contract_group_routes.end() && !group_it->second.empty()) {
                    continue;
                }
                emit_replay(contract->condition_id, contract->asset_name, *contract, 0, 1);
            }
        }

        const float parse_us = config.metrics_enabled
            ? static_cast<float>(std::max<int64_t>(
                  0, static_cast<int64_t>(t2_parse_done - t1_parse_start - frame.book_apply_ns)) / 1000.0)
            : 0.0f;
        const float book_us = config.metrics_enabled
            ? static_cast<float>(frame.book_apply_ns / 1000.0)
            : 0.0f;
        const float arb_us = (config.metrics_enabled && arb_checks > 0)
            ? static_cast<float>((t4_arb_done - t3_books_done) / 1000.0)
            : 0.0f;
        const float e2e_us = (config.metrics_enabled && arb_checks > 0)
            ? static_cast<float>((t4_arb_done - slot->recv_time) / 1000.0)
            : 0.0f;
        // Feed-delivery latency: wall-clock at receipt minus the newest exchange
        // timestamp in this frame. This is the geography signal (network + their
        // origin processing), the dominant lever; it includes any clock skew
        // between our box and Polymarket, so keep clocks chrony/NTP-synced.
        const bool have_feed_delivery = frame.max_event_ts > 0 && slot->recv_wall_ns > 0;
        const float feed_delivery_ms = have_feed_delivery
            ? static_cast<float>(static_cast<double>(slot->recv_wall_ns) / 1e6
                                 - static_cast<double>(frame.max_event_ts))
            : 0.0f;

        auto push_frame_metric = [&](MetricsEventType type, uint16_t count, bool attach_latency_sample) {
            if (count == 0) return;
            MetricsEvent event{};
            event.type = type;
            event.event_count = count;
            event.has_latency_sample = attach_latency_sample;
            event.queue_us = queue_us;
            event.parse_us = parse_us;
            event.book_us = book_us;
            event.arb_us = arb_us;
            event.e2e_us = e2e_us;
            event.feed_delivery_ms = feed_delivery_ms;
            event.has_feed_delivery = attach_latency_sample && have_feed_delivery;
            event.arb_checks = attach_latency_sample ? arb_checks : 0;
            event.arb_checked = attach_latency_sample && arb_checks > 0;
            push_metrics(event);
        };

        bool latency_attached = false;
        auto take_latency_slot = [&](uint16_t count) {
            if (count == 0 || latency_attached) return false;
            latency_attached = true;
            return true;
        };

        push_frame_metric(MetricsEventType::BOOK, frame.book_events, take_latency_slot(frame.book_events));
        push_frame_metric(MetricsEventType::PRICE_CHANGE, frame.price_change_events,
                          take_latency_slot(frame.price_change_events));
        push_frame_metric(MetricsEventType::BEST_BID_ASK, frame.bbo_events,
                          take_latency_slot(frame.bbo_events));
        if (frame.trade_events > 0) {
            MetricsEvent trade_event{};
            trade_event.type = MetricsEventType::TRADE;
            trade_event.event_count = frame.trade_events;
            push_metrics(trade_event);
        }

        message_queue->pop();
    }

    if (config.shadow_executor_enabled) {
        shadow_oms.cancel_everything();  // flat on shutdown (enqueues final cancels)
    }
    sender_stop.store(true, std::memory_order_release);  // let sender drain + exit

    g_running = false;

    if (receiver_thread.joinable()) {
        receiver_thread.join();
    }
    if (sender_thread.joinable()) {
        sender_thread.join();
    }
    if (reward_refresh_thread.joinable()) {
        reward_refresh_thread.join();
    }
    if (logger_thread.joinable()) {
        logger_thread.join();
    }

    if (config.shadow_executor_enabled) {
        const OmsStats& os = shadow_oms.stats();
        const double avg_qmin = shadow_qmin_count ? shadow_qmin_sum / shadow_qmin_count : 0.0;
        const auto rstat = acr_react_us.compute();
        const auto sstat = sender_send_us.compute();
        std::printf("\n[SHADOW-OMS] creates=%" PRIu64 " cancels=%" PRIu64 " replaces=%" PRIu64
                    " risk_rejects=%" PRIu64 " fills=%" PRIu64 "\n",
                    os.creates, os.cancels, os.replaces, os.risk_rejects, os.fills);
        std::printf("[SHADOW-OMS] reward telemetry: avg per-quote Qmin=%.1f over %" PRIu64
                    " eligible quotes\n", avg_qmin, shadow_qmin_count);
        std::printf("[ACR] at-risk events=%" PRIu64 " | react (trigger->detect) p50=%.2fus p99=%.2fus max=%.2fus\n",
                    acr_events, rstat.p50, rstat.p99, rstat.max);
        std::printf("[ACR] sender exec: creates=%" PRIu64 " cancels=%" PRIu64
                    " | enqueue->send p50=%.2fus p99=%.2fus max=%.2fus\n",
                    sender_exec_creates, sender_exec_cancels, sstat.p50, sstat.p99, sstat.max);
        std::printf("[ACR] NOTE: react/send are IN-PROCESS only. Real cancel-race win = these +"
                    " network RTT to the London engine (~10-15ms from eu-west-1). No orders sent.\n");
        if (config.quote_throttle_enabled) {
            std::printf("[THROTTLE] reconciles suppressed (queue-priority hold)=%" PRIu64 "\n",
                        throttle_skips);
        }
        if (config.dead_mans_switch_seconds > 0) {
            std::printf("[DMS] dead-man's-switch trips (feed-stale flatten)=%" PRIu64
                        " | armed at %ds stale\n", dms_trips, config.dead_mans_switch_seconds);
        }
        if (exec_mode == ExecMode::MockLive) {
            const live::MockLiveStats& ms = mock_exec.stats();
            std::printf("[MOCKLIVE] EIP-712 digests computed=%" PRIu64 " (creates) cancels=%" PRIu64
                        " | last digest=%s\n", ms.digests, ms.cancels,
                        eip712::to_hex(ms.last_digest).c_str());
            std::printf("[MOCKLIVE] near-miss-live events (off-grid/too-wide/crossed)=%" PRIu64
                        " -> %s | telemetry drops=%" PRIu64 "\n",
                        near_miss_live ? near_miss_live->count() : 0,
                        config.near_miss_live_log_file.c_str(),
                        dropped_quote_telemetry.load(std::memory_order_relaxed));
        }
    }

    const uint64_t oversize_drops = dropped_oversize_messages.load(std::memory_order_relaxed);
    const uint64_t metrics_drops = dropped_metrics_events.load(std::memory_order_relaxed);
    const uint64_t opportunity_drops = dropped_opportunities.load(std::memory_order_relaxed);
    const uint64_t replay_drops = dropped_replay_snapshots.load(std::memory_order_relaxed);
    const uint64_t backpressure_events = message_queue_backpressure_events.load(std::memory_order_relaxed);
    const uint64_t backpressure_spins = message_queue_backpressure_spins.load(std::memory_order_relaxed);
    const uint64_t max_message_bytes = max_message_bytes_seen.load(std::memory_order_relaxed);
    const uint64_t max_frame_opportunities = max_frame_opportunities_seen.load(std::memory_order_relaxed);

    std::printf("[PIPELINE] Queue peaks msg=%zu/%zu metrics=%zu/%zu opp=%zu/%zu replay=%zu/%zu\n",
                message_queue->peak_size(), message_queue->capacity(),
                metrics_queue->peak_size(), metrics_queue->capacity(),
                opportunity_queue->peak_size(), opportunity_queue->capacity(),
                replay_queue->peak_size(), replay_queue->capacity());
    std::printf("[PIPELINE] Max frame=%" PRIu64 " bytes | max frame opps=%" PRIu64
                " | msg backpressure=%" PRIu64 " events (%" PRIu64 " spins)\n",
                max_message_bytes, max_frame_opportunities, backpressure_events, backpressure_spins);
    std::printf("[PIPELINE] Dropped oversized=%" PRIu64
                " | dropped metrics=%" PRIu64
                " | dropped opportunities=%" PRIu64
                " | dropped replay=%" PRIu64 "\n",
                oversize_drops, metrics_drops, opportunity_drops, replay_drops);
    std::printf("[WEBSOCKET] attempts=%" PRIu64 " success=%" PRIu64
                " reconnects=%" PRIu64 " stale=%" PRIu64
                " timeouts=%" PRIu64 " read_errors=%" PRIu64
                " closes=%" PRIu64 " pings=%" PRIu64 "\n",
                websocket_stats.connect_attempts,
                websocket_stats.successful_connects,
                websocket_stats.reconnect_cycles,
                websocket_stats.stale_reconnects,
                websocket_stats.timeout_polls,
                websocket_stats.read_errors,
                websocket_stats.closed_events,
                websocket_stats.ping_count);

    std::vector<const Contract*> market_activity;
    market_activity.reserve(config.contracts.size());
    std::vector<const Contract*> zero_update_markets;
    zero_update_markets.reserve(config.contracts.size());
    for (const auto& contract : config.contracts) {
        market_activity.push_back(&contract);
        if (contract.total_updates == 0) {
            zero_update_markets.push_back(&contract);
        }
    }
    std::sort(market_activity.begin(), market_activity.end(),
              [](const Contract* lhs, const Contract* rhs) {
                  if (lhs->total_updates != rhs->total_updates) {
                      return lhs->total_updates > rhs->total_updates;
                  }
                  return lhs->asset_name < rhs->asset_name;
              });

    std::printf("[FEED] Markets with updates=%zu/%zu | zero-update=%zu\n",
                config.contracts.size() - zero_update_markets.size(),
                config.contracts.size(),
                zero_update_markets.size());
    const size_t report_limit = std::min(config.active_market_report_limit, market_activity.size());
    for (size_t i = 0; i < report_limit; ++i) {
        const Contract* contract = market_activity[i];
        if (contract->total_updates == 0) break;
        std::printf("[FEED] Top %-2zu %-40s | total=%" PRIu64 " yes=%" PRIu64 " no=%" PRIu64 "\n",
                    i + 1,
                    contract->asset_name.c_str(),
                    contract->total_updates,
                    contract->yes_updates,
                    contract->no_updates);
    }
    if (!zero_update_markets.empty()) {
        std::printf("[FEED] Zero-update markets:");
        const size_t zero_report_limit = std::min(config.active_market_report_limit, zero_update_markets.size());
        for (size_t i = 0; i < zero_report_limit; ++i) {
            std::printf(" %s%s",
                        zero_update_markets[i]->asset_name.c_str(),
                        i + 1 == zero_report_limit ? "" : ",");
        }
        if (zero_update_markets.size() > zero_report_limit) {
            std::printf(" ...");
        }
        std::printf("\n");
    }

    return 0;
}
