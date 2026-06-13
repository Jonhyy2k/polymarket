#include "market_metadata.hpp"

#include "http_json.hpp"

#include <simdjson.h>

#include <cmath>
#include <exception>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string sanitize_cache_field(std::string_view input) {
    std::string out;
    out.reserve(input.size());
    for (char ch : input) {
        if (ch == '\t' || ch == '\n' || ch == '\r') {
            out.push_back(' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

std::vector<std::string> split_tsv_line(const std::string& line) {
    std::vector<std::string> parts;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, '\t')) {
        parts.push_back(std::move(field));
    }
    if (!line.empty() && line.back() == '\t') {
        parts.emplace_back();
    }
    return parts;
}

bool parse_market_metadata(std::string_view json, MarketMetadata& out, std::string& error) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);

    simdjson::ondemand::array markets;
    if (doc.get_array().get(markets)) {
        error = "Gamma market payload was not an array";
        return false;
    }

    for (auto elem : markets) {
        simdjson::ondemand::object obj;
        if (elem.get_object().get(obj)) continue;

        bool bv = false;
        int64_t iv = 0;
        double dv = 0.0;
        std::string_view sv;

        out.available = true;
        if (!obj["feesEnabled"].get_bool().get(bv)) out.fees_enabled = bv;
        if (!obj["negRisk"].get_bool().get(bv)) out.neg_risk = bv;
        if (!obj["takerBaseFee"].get_int64().get(iv)) out.base_fee_bps = static_cast<int>(iv);
        // gamma sends orderPriceMinTickSize as a JSON number (e.g. 0.001).
        if (!obj["orderPriceMinTickSize"].get_double().get(dv)) out.min_tick_size = dv;

        simdjson::ondemand::object fee_schedule;
        if (!obj["feeSchedule"].get_object().get(fee_schedule)) {
            // Positively parsed a fee schedule: rate present => mark valid so the
            // detector can fail closed when this flag is absent.
            if (!fee_schedule["rate"].get_double().get(dv)) {
                out.fee_rate = dv;
                out.fee_schedule_valid = true;
            }
            if (!fee_schedule["exponent"].get_int64().get(iv)) {
                out.fee_exponent = static_cast<int>(iv);
            }
            if (!fee_schedule["rebateRate"].get_double().get(dv)) out.fee_rebate_rate = dv;
            if (!fee_schedule["takerOnly"].get_bool().get(bv)) out.fee_taker_only = bv;
        }

        simdjson::ondemand::array events;
        if (!obj["events"].get_array().get(events)) {
            for (auto event_elem : events) {
                simdjson::ondemand::object event_obj;
                if (event_elem.get_object().get(event_obj)) continue;
                if (!event_obj["slug"].get_string().get(sv)) out.event_slug = std::string(sv);
                if (!event_obj["title"].get_string().get(sv)) out.event_title = std::string(sv);
                break;
            }
        }

        return true;
    }

    error = "No market metadata returned";
    return false;
}

bool parse_reward_config(std::string_view json, RewardConfigRaw& out, std::string& error) {
    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);

    simdjson::ondemand::object obj;
    if (doc.get_object().get(obj)) {
        error = "CLOB market payload was not an object";
        return false;
    }

    double dv = 0.0;
    if (!obj["minimum_tick_size"].get_double().get(dv)) {
        out.tick_thou = static_cast<int>(std::llround(dv * 1000.0));
    }

    simdjson::ondemand::object rewards;
    if (obj["rewards"].get_object().get(rewards)) {
        // No rewards object => not reward-eligible (leave out.active = false).
        return true;
    }

    if (!rewards["min_size"].get_double().get(dv)) out.min_size = static_cast<int>(std::llround(dv));
    if (!rewards["max_spread"].get_double().get(dv)) {
        // max_spread is in CENTS (e.g. 4.5) -> thousandths (45).
        out.max_spread_thou = static_cast<int>(std::llround(dv * 10.0));
    }

    // rates is null when the market is not currently emitting rewards.
    simdjson::ondemand::value rates_val;
    if (!rewards["rates"].get(rates_val)) {
        simdjson::ondemand::json_type t;
        if (!rates_val.type().get(t) && t == simdjson::ondemand::json_type::array) {
            simdjson::ondemand::array rates;
            if (!rates_val.get_array().get(rates)) {
                for (auto rate_elem : rates) {
                    simdjson::ondemand::object rate_obj;
                    if (rate_elem.get_object().get(rate_obj)) continue;
                    double rate = 0.0;
                    if (!rate_obj["rewards_daily_rate"].get_double().get(rate)) {
                        out.daily_rate_usd += rate;
                        out.active = true;
                    }
                }
            }
        }
    }
    return true;
}

bool parse_event_market_count(std::string_view json, size_t& market_count, std::string& error) {
    market_count = 0;

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded(json);
    auto doc = parser.iterate(padded);

    simdjson::ondemand::array events;
    if (doc.get_array().get(events)) {
        error = "Gamma event payload was not an array";
        return false;
    }

    for (auto elem : events) {
        simdjson::ondemand::object obj;
        if (elem.get_object().get(obj)) continue;

        simdjson::ondemand::array markets;
        if (obj["markets"].get_array().get(markets)) {
            error = "Event did not include a markets array";
            return false;
        }

        size_t count = 0;
        for (auto ignored : markets) {
            (void)ignored;
            ++count;
        }
        market_count = count;
        return true;
    }

    error = "No event metadata returned";
    return false;
}

}  // namespace

bool load_market_metadata_cache(const std::string& path, MarketMetadataCache& cache, std::string& error) {
    cache.markets_by_token.clear();
    cache.event_market_counts.clear();
    if (path.empty()) return true;

    std::ifstream input(path);
    if (!input.is_open()) {
        return true;
    }

    std::string line;
    if (!std::getline(input, line)) {
        return true;
    }

    try {
        while (std::getline(input, line)) {
            if (line.empty()) continue;
            const auto fields = split_tsv_line(line);
            if (fields.empty()) continue;

            if (fields[0] == "market") {
                if (fields.size() < 9 || fields[1].empty()) continue;
                MarketMetadata metadata;
                metadata.available = true;
                metadata.fees_enabled = fields[2] == "1";
                metadata.fee_rate = std::stod(fields[3]);
                metadata.fee_exponent = std::stoi(fields[4]);
                metadata.base_fee_bps = std::stoi(fields[5]);
                metadata.neg_risk = fields[6] == "1";
                metadata.event_slug = fields[7];
                metadata.event_title = fields[8];
                if (fields.size() >= 13) {
                    metadata.min_tick_size = std::stod(fields[9]);
                    metadata.fee_rebate_rate = std::stod(fields[10]);
                    metadata.fee_taker_only = fields[11] == "1";
                    metadata.fee_schedule_valid = fields[12] == "1";
                }
                cache.markets_by_token.emplace(fields[1], std::move(metadata));
            } else if (fields[0] == "event") {
                if (fields.size() < 3 || fields[1].empty()) continue;
                cache.event_market_counts.emplace(fields[1], static_cast<size_t>(std::stoull(fields[2])));
            }
        }
    } catch (const std::exception& ex) {
        error = ex.what();
        cache.markets_by_token.clear();
        cache.event_market_counts.clear();
        return false;
    }

    return true;
}

bool save_market_metadata_cache(const std::string& path, const MarketMetadataCache& cache, std::string& error) {
    if (path.empty()) return true;

    std::ofstream output(path, std::ios::trunc);
    if (!output.is_open()) {
        error = "Could not open metadata cache for write: " + path;
        return false;
    }

    output << "kind\tkey\tv1\tv2\tv3\tv4\tv5\tv6\tv7\tv8\tv9\tv10\tv11\n";
    for (const auto& [token_id, metadata] : cache.markets_by_token) {
        output << "market\t"
               << sanitize_cache_field(token_id) << "\t"
               << (metadata.fees_enabled ? 1 : 0) << "\t"
               << metadata.fee_rate << "\t"
               << metadata.fee_exponent << "\t"
               << metadata.base_fee_bps << "\t"
               << (metadata.neg_risk ? 1 : 0) << "\t"
               << sanitize_cache_field(metadata.event_slug) << "\t"
               << sanitize_cache_field(metadata.event_title) << "\t"
               << metadata.min_tick_size << "\t"
               << metadata.fee_rebate_rate << "\t"
               << (metadata.fee_taker_only ? 1 : 0) << "\t"
               << (metadata.fee_schedule_valid ? 1 : 0) << "\n";
    }
    for (const auto& [event_slug, market_count] : cache.event_market_counts) {
        output << "event\t"
               << sanitize_cache_field(event_slug) << "\t"
               << market_count << "\n";
    }
    return true;
}

ClobRewardsClient::ClobRewardsClient()
    : session_(std::make_unique<HttpsSession>("clob.polymarket.com")) {}
ClobRewardsClient::~ClobRewardsClient() = default;
ClobRewardsClient::ClobRewardsClient(ClobRewardsClient&&) noexcept = default;
ClobRewardsClient& ClobRewardsClient::operator=(ClobRewardsClient&&) noexcept = default;

bool ClobRewardsClient::fetch(const std::string& condition_id, RewardConfigRaw& out,
                              std::string& error) {
    std::string body;
    std::string target = "/markets/";
    target.append(condition_id);
    if (!session_->get(target, body, error, nullptr)) {
        return false;
    }
    return parse_reward_config(body, out, error);
}

MarketMetadataClient::MarketMetadataClient()
    : session_(std::make_unique<HttpsSession>("gamma-api.polymarket.com")) {}

MarketMetadataClient::~MarketMetadataClient() = default;

MarketMetadataClient::MarketMetadataClient(MarketMetadataClient&&) noexcept = default;

MarketMetadataClient& MarketMetadataClient::operator=(MarketMetadataClient&&) noexcept = default;

bool MarketMetadataClient::https_get(std::string_view target, std::string& body, std::string& error) {
    return session_->get(target, body, error, nullptr);
}

bool MarketMetadataClient::fetch_market_by_token(std::string_view token_id, MarketMetadata& out,
                                                 std::string& error) {
    std::string body;
    std::string target = "/markets?clob_token_ids=";
    target.append(token_id.data(), token_id.size());
    if (!https_get(target, body, error)) {
        return false;
    }
    return parse_market_metadata(body, out, error);
}

bool MarketMetadataClient::fetch_event_market_count(std::string_view event_slug, size_t& market_count,
                                                    std::string& error) {
    std::string body;
    std::string target = "/events?slug=";
    target.append(event_slug.data(), event_slug.size());
    if (!https_get(target, body, error)) {
        return false;
    }
    return parse_event_market_count(body, market_count, error);
}
