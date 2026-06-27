#pragma once

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ LiveGateway — the real Polymarket CLOB v2 execution path.                   │
// │                                                                            │
// │ Implements IExecGateway, so the OMS / reconcile / risk / ACR code is        │
// │ unchanged (mode-agnostic). On submit it builds the v2 order, computes the   │
// │ EIP-712 digest, ECDSA-signs it with the EOA key, serializes the exact       │
// │ orderToJsonV2 wire body, attaches the L2 HMAC headers, and POSTs /order.    │
// │ All four sub-steps are individually cross-checked vs eth_account /          │
// │ @polymarket/clob-client-v2 in live_test (digest + signature + wire body).   │
// │                                                                            │
// │ SAFETY                                                                      │
// │  • The EOA private key and L2 secret are loaded from the ENVIRONMENT by the │
// │    caller and handed in here; they are never logged, printed, or persisted. │
// │  • `arm == false` (the default) builds + signs but DOES NOT POST — a dry    │
// │    run that exercises the whole path with zero money at risk.               │
// │  • preflight() refuses to arm unless GET /version matches the version this  │
// │    binary was built for, the keccak provider is correct, creds + key are    │
// │    present, and address(key) == the configured signer.                      │
// │  • submit/cancel/adopt all run on the single cancel-sender thread, so the   │
// │    client_id→exchange_order_id map needs no lock.                            │
// └────────────────────────────────────────────────────────────────────────────┘

#include "oms.hpp"          // IExecGateway, ManagedOrder, OrderSide
#include "live_order.hpp"   // SignerConfig, make_payload, digest, wire_body
#include "clob_auth.hpp"    // L2Creds, build_l2_headers
#include "http_json.hpp"    // HttpsSession
#include "eip712_sign.hpp"  // sign_digest, address_from_privkey

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace live {

struct LiveConfig {
    std::string  host = "clob.polymarket.com";
    std::string  port = "443";
    clob::L2Creds creds;                 // api_key/secret/passphrase/address (from env)
    eip712::Bytes32 priv{};              // EOA private key bytes (from env)
    bool         priv_loaded = false;
    SignerConfig signer;                 // maker / signer / signature_type
    std::string  order_type = "GTC";
    bool         arm = false;            // false => sign but DO NOT POST (dry run)
    int          expected_version = 2;   // preflight asserts GET /version == this
};

struct LiveStats {
    uint64_t submits = 0, submit_ok = 0, submit_err = 0, submit_dry = 0;
    uint64_t cancels = 0, cancel_ok = 0, cancel_err = 0, cancel_unknown = 0;
    uint64_t sign_count = 0;
    double   sign_us_total = 0.0;
    double   last_post_ms = 0.0;
    std::string last_error;             // last non-2xx body / transport error (no secrets)
};

class LiveGateway : public IExecGateway {
public:
    LiveGateway(LiveConfig cfg, bool verbose);

    bool submit(const ManagedOrder& order) override;
    bool cancel(const ManagedOrder& order) override;
    // Fetch GET /data/orders and return the account's resting orders. asset_id
    // strings are interned in this gateway (outlives the OMS), so the returned
    // ManagedOrder::token_id views are stable per the interface contract.
    bool adopt_open_orders(std::vector<ManagedOrder>& out) override;

    // Cancel every resting order (DELETE /cancel-all). Used at startup to clear
    // orphans from a previous run before quoting. Returns true on HTTP 200.
    bool cancel_all();

    // Safety gate before arming. On false, `reason` explains why (caller stays in
    // dry-run / shadow). Does a live GET /version and GET /time round-trip.
    bool preflight(std::string& reason);

    const LiveStats& stats() const noexcept { return stats_; }
    bool armed() const noexcept { return cfg_.arm; }

private:
    LiveConfig                    cfg_;
    bool                          verbose_;
    uint64_t                      next_salt_;
    std::unique_ptr<HttpsSession> http_;
    LiveStats                     stats_;

    // client_id -> exchange order id (learned at submit, used at cancel). Single
    // sender thread => no lock. Small N (a few resting orders), linear scan.
    std::vector<std::pair<uint64_t, std::string>> id_map_;
    std::deque<std::string>       interned_;   // stable storage for adopted token ids

    void               remember(uint64_t client_id, std::string exchange_order_id);
    const std::string* lookup(uint64_t client_id) const;
    void               forget(uint64_t client_id);
};

}  // namespace live
