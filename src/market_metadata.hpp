#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

struct MarketMetadata {
    bool available = false;
    bool fees_enabled = false;
    bool fee_schedule_valid = false;   // true only when feeSchedule was positively parsed
    double fee_rate = 0.0;
    double fee_rebate_rate = 0.0;
    bool fee_taker_only = true;
    int fee_exponent = 0;
    int base_fee_bps = -1;
    double min_tick_size = 0.0;        // gamma orderPriceMinTickSize (e.g. 0.001, 0.01)
    bool neg_risk = false;
    std::string event_slug;
    std::string event_title;
};

struct MarketMetadataCache {
    std::unordered_map<std::string, MarketMetadata> markets_by_token;
    std::unordered_map<std::string, size_t> event_market_counts;
};

bool load_market_metadata_cache(const std::string& path, MarketMetadataCache& cache, std::string& error);
bool save_market_metadata_cache(const std::string& path, const MarketMetadataCache& cache, std::string& error);

class HttpsSession;

// Liquidity-rewards config fetched from the CLOB (GET /markets/{condition_id}).
struct RewardConfigRaw {
    bool   active = false;
    int    max_spread_thou = 0;   // max_spread cents * 10
    int    min_size = 0;
    double daily_rate_usd = 0.0;
    int    tick_thou = 0;         // minimum_tick_size * 1000 (sanity cross-check)
};

// Persistent session to clob.polymarket.com for reward config (read-only).
class ClobRewardsClient {
public:
    ClobRewardsClient();
    ~ClobRewardsClient();
    ClobRewardsClient(ClobRewardsClient&&) noexcept;
    ClobRewardsClient& operator=(ClobRewardsClient&&) noexcept;
    ClobRewardsClient(const ClobRewardsClient&) = delete;
    ClobRewardsClient& operator=(const ClobRewardsClient&) = delete;

    bool fetch(const std::string& condition_id, RewardConfigRaw& out, std::string& error);

private:
    std::unique_ptr<HttpsSession> session_;
};

class MarketMetadataClient {
public:
    MarketMetadataClient();
    ~MarketMetadataClient();

    MarketMetadataClient(MarketMetadataClient&&) noexcept;
    MarketMetadataClient& operator=(MarketMetadataClient&&) noexcept;
    MarketMetadataClient(const MarketMetadataClient&) = delete;
    MarketMetadataClient& operator=(const MarketMetadataClient&) = delete;

    bool fetch_market_by_token(std::string_view token_id, MarketMetadata& out, std::string& error);
    bool fetch_event_market_count(std::string_view event_slug, size_t& market_count, std::string& error);

private:
    bool https_get(std::string_view target, std::string& body, std::string& error);

    std::unique_ptr<HttpsSession> session_;
};
