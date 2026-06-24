#include "mock_live_gateway.hpp"

#include <cstdio>

namespace live {

MockLiveGateway::MockLiveGateway(SignerConfig cfg, bool verbose)
    : cfg_(std::move(cfg)),
      verbose_(verbose),
      next_salt_(static_cast<uint64_t>(now_realtime_ns())) {}

bool MockLiveGateway::submit(const ManagedOrder& order) {
    const bool is_buy = (order.side == OrderSide::BUY);
    const uint64_t salt = next_salt_++;
    const uint64_t ts_ms = static_cast<uint64_t>(now_realtime_ns() / 1000000ull);

    const LiveOrderPayload p = make_payload(cfg_, order.token_id, is_buy,
                                            order.price, order.size, salt, ts_ms,
                                            order.neg_risk);
    stats_.last_digest = digest(p);   // full EIP-712 typed-data hash (unsigned)
    ++stats_.digests;
    ++stats_.submits;

    if (verbose_) {
        std::printf("[MOCKLIVE] CREATE  cid=%llu %-4s %.3f x %u  %s  digest=%s%.16s\n",
                    static_cast<unsigned long long>(order.client_id),
                    order_side_name(order.side),
                    static_cast<double>(order.price) / 1000.0, order.size,
                    order.neg_risk ? "NEGRISK" : "STD",
                    "0x", eip712::to_hex(stats_.last_digest).c_str() + 2);
    }
    return true;
}

bool MockLiveGateway::cancel(const ManagedOrder& order) {
    ++stats_.cancels;
    if (verbose_) {
        std::printf("[MOCKLIVE] CANCEL  cid=%llu %-4s %.3f x %u\n",
                    static_cast<unsigned long long>(order.client_id),
                    order_side_name(order.side),
                    static_cast<double>(order.price) / 1000.0, order.size);
    }
    return true;
}

void MockLiveGateway::describe() {
    const eip712::Domain std_dom = eip712::polymarket_v2::standard();
    const eip712::Domain neg_dom = eip712::polymarket_v2::neg_risk();
    std::printf("[MOCKLIVE] keccak self-test: %s\n", eip712::self_test() ? "PASS" : "FAIL");
    std::printf("[MOCKLIVE] ORDER_TYPEHASH = %s\n",
                eip712::to_hex(eip712::order_typehash()).c_str());
    std::printf("[MOCKLIVE] domain[std]  name=\"%s\" v=%s chainId=%llu vc=%s\n",
                std_dom.name.c_str(), std_dom.version.c_str(),
                static_cast<unsigned long long>(std_dom.chain_id),
                std_dom.verifying_contract.c_str());
    std::printf("[MOCKLIVE]   sep[std]  = %s\n",
                eip712::to_hex(eip712::domain_separator(std_dom)).c_str());
    std::printf("[MOCKLIVE] domain[neg]  name=\"%s\" v=%s chainId=%llu vc=%s\n",
                neg_dom.name.c_str(), neg_dom.version.c_str(),
                static_cast<unsigned long long>(neg_dom.chain_id),
                neg_dom.verifying_contract.c_str());
    std::printf("[MOCKLIVE]   sep[neg]  = %s\n",
                eip712::to_hex(eip712::domain_separator(neg_dom)).c_str());
    std::printf("[MOCKLIVE] *** EIP-712 params are UNVERIFIED vs the live on-chain "
                "Exchange — digests are for path validation only, NOT signing. ***\n");
}

// ── NearMissLiveLog ─────────────────────────────────────────────────────────

NearMissLiveLog::NearMissLiveLog(const std::string& path) : file_(path, std::ios::out) {
    if (file_) {
        file_ << "timestamp_ns,token,mid_thou,tick_thou,bid_px,ask_px,"
                 "off_grid,too_wide,crossed\n";
        file_.flush();
    }
}

void NearMissLiveLog::record(std::string_view token, uint32_t mid2, Price tick,
                             const DesiredQuotes& q, const QuoteIssues& issues) {
    if (!file_ || !issues.any()) return;
    ++count_;
    const unsigned bid_px = q.bid.valid ? q.bid.price : 0;
    const unsigned ask_px = q.ask.valid ? q.ask.price : 0;
    file_ << now_realtime_ns() << ','
          << token << ','
          << (mid2 / 2) << ','
          << tick << ','
          << bid_px << ',' << ask_px << ','
          << (issues.off_grid ? 1 : 0) << ','
          << (issues.too_wide ? 1 : 0) << ','
          << (issues.crossed ? 1 : 0) << '\n';
    file_.flush();  // rare event; keep it durable
}

}  // namespace live
