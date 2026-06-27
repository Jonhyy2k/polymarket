#include "live_gateway.hpp"

#include "types.hpp"   // now_realtime_ns

#include <simdjson.h>

#include <cstdio>

namespace live {

namespace {

// Parse an OrderResponse (POST /order). Returns true if the order was accepted;
// fills order_id/status. On rejection, fills err with the server message.
bool parse_order_response(const std::string& body, std::string& order_id,
                          std::string& status, std::string& err) {
    order_id.clear(); status.clear(); err.clear();
    simdjson::dom::parser p;
    simdjson::dom::element doc;
    if (p.parse(body).get(doc) != simdjson::SUCCESS) { err = "unparseable response"; return false; }

    // Error shapes: {"error":"..."} or {"success":false,"errorMsg":"..."}.
    std::string_view sv;
    if (doc["error"].get_string().get(sv) == simdjson::SUCCESS && !sv.empty()) {
        err.assign(sv); return false;
    }
    bool success = false;
    const bool has_success = (doc["success"].get_bool().get(success) == simdjson::SUCCESS);
    if (doc["orderID"].get_string().get(sv) == simdjson::SUCCESS) order_id.assign(sv);
    if (doc["status"].get_string().get(sv)  == simdjson::SUCCESS) status.assign(sv);
    if (has_success && !success) {
        if (doc["errorMsg"].get_string().get(sv) == simdjson::SUCCESS) err.assign(sv);
        else err = "order rejected";
        return false;
    }
    // Accepted if we got an orderID (or an explicit success:true).
    if (!order_id.empty() || (has_success && success)) return true;
    err = "no orderID in response";
    return false;
}

}  // namespace

LiveGateway::LiveGateway(LiveConfig cfg, bool verbose)
    : cfg_(std::move(cfg)),
      verbose_(verbose),
      next_salt_(static_cast<uint64_t>(now_realtime_ns())),
      http_(std::make_unique<HttpsSession>(cfg_.host, cfg_.port)) {}

void LiveGateway::remember(uint64_t client_id, std::string exchange_order_id) {
    for (auto& kv : id_map_) {
        if (kv.first == client_id) { kv.second = std::move(exchange_order_id); return; }
    }
    id_map_.emplace_back(client_id, std::move(exchange_order_id));
}

const std::string* LiveGateway::lookup(uint64_t client_id) const {
    for (const auto& kv : id_map_) if (kv.first == client_id) return &kv.second;
    return nullptr;
}

void LiveGateway::forget(uint64_t client_id) {
    for (auto it = id_map_.begin(); it != id_map_.end(); ++it) {
        if (it->first == client_id) { id_map_.erase(it); return; }
    }
}

bool LiveGateway::submit(const ManagedOrder& order) {
    ++stats_.submits;
    const bool     is_buy = (order.side == OrderSide::BUY);
    const uint64_t salt   = next_salt_++;
    const uint64_t ts_ms  = static_cast<uint64_t>(now_realtime_ns() / 1000000ull);

    const LiveOrderPayload p = make_payload(cfg_.signer, order.token_id, is_buy,
                                            order.price, order.size, salt, ts_ms,
                                            order.neg_risk);

    // sign the EIP-712 digest with the EOA key
    const NanoTime t0 = now_ns();
    const eip712::Bytes32 d = digest(p);
    eip712::Sig65 sig{};
    if (!cfg_.priv_loaded || !eip712::sign_digest(d, cfg_.priv, sig)) {
        ++stats_.submit_err;
        stats_.last_error = "sign failed (no key?)";
        if (verbose_) std::printf("[LIVE] CREATE cid=%llu SIGN FAILED\n",
                                  (unsigned long long)order.client_id);
        return false;
    }
    ++stats_.sign_count;
    stats_.sign_us_total += static_cast<double>(now_ns() - t0) / 1000.0;

    const std::string sig_hex = eip712::sig_to_hex(sig);
    const std::string body    = wire_body(p, sig_hex, cfg_.creds.api_key, cfg_.order_type);

    if (!cfg_.arm) {                       // dry run: built + signed, but no POST
        ++stats_.submit_dry;
        if (verbose_) std::printf("[LIVE-DRY] CREATE cid=%llu %-4s %.3f x%u %s digest=0x%.16s\n",
                                  (unsigned long long)order.client_id, order_side_name(order.side),
                                  order.price / 1000.0, order.size, order.neg_risk ? "NEG" : "STD",
                                  eip712::to_hex(d).c_str() + 2);
        return true;
    }

    const long ts = clob::unix_now();
    const HttpHeaders headers = clob::build_l2_headers(cfg_.creds, ts, "POST", "/order", body);

    HttpResponse resp; std::string err; double ms = 0.0;
    if (!http_->request("POST", "/order", body, headers, resp, err, "application/json", &ms)) {
        ++stats_.submit_err; stats_.last_error = err;
        if (verbose_) std::printf("[LIVE] CREATE cid=%llu TRANSPORT ERR: %s\n",
                                  (unsigned long long)order.client_id, err.c_str());
        return false;
    }
    stats_.last_post_ms = ms;

    std::string oid, status, perr;
    const bool ok = parse_order_response(resp.body, oid, status, perr);
    if (ok) {
        ++stats_.submit_ok;
        if (!oid.empty()) remember(order.client_id, oid);
        if (verbose_) std::printf("[LIVE] CREATE cid=%llu OK status=%s id=%.18s (%.1fms)\n",
                                  (unsigned long long)order.client_id, status.c_str(),
                                  oid.c_str(), ms);
        return true;
    }
    ++stats_.submit_err;
    stats_.last_error = perr.empty() ? resp.body : perr;
    if (verbose_) std::printf("[LIVE] CREATE cid=%llu REJECT http=%d: %s\n",
                              (unsigned long long)order.client_id, resp.status,
                              stats_.last_error.c_str());
    return false;
}

bool LiveGateway::cancel(const ManagedOrder& order) {
    ++stats_.cancels;
    const std::string* oid = lookup(order.client_id);
    if (!oid && !order.exchange_order_id.empty()) oid = &order.exchange_order_id;
    if (!oid || oid->empty()) {
        ++stats_.cancel_unknown;
        if (verbose_) std::printf("[LIVE] CANCEL cid=%llu UNKNOWN (no exchange id)\n",
                                  (unsigned long long)order.client_id);
        return false;
    }

    std::string body = "{\"orderID\":\"";
    body += *oid;
    body += "\"}";

    if (!cfg_.arm) {                       // dry run
        if (verbose_) std::printf("[LIVE-DRY] CANCEL cid=%llu id=%.18s\n",
                                  (unsigned long long)order.client_id, oid->c_str());
        return true;
    }

    const long ts = clob::unix_now();
    const HttpHeaders headers = clob::build_l2_headers(cfg_.creds, ts, "DELETE", "/order", body);

    HttpResponse resp; std::string err;
    if (!http_->request("DELETE", "/order", body, headers, resp, err)) {
        ++stats_.cancel_err; stats_.last_error = err;
        return false;
    }
    if (resp.status >= 200 && resp.status < 300) {
        ++stats_.cancel_ok;
        forget(order.client_id);
        if (verbose_) std::printf("[LIVE] CANCEL cid=%llu OK\n",
                                  (unsigned long long)order.client_id);
        return true;
    }
    ++stats_.cancel_err;
    stats_.last_error = resp.body;
    if (verbose_) std::printf("[LIVE] CANCEL cid=%llu REJECT http=%d: %s\n",
                              (unsigned long long)order.client_id, resp.status, resp.body.c_str());
    return false;
}

bool LiveGateway::cancel_all() {
    if (!cfg_.arm) return true;            // nothing resting in dry run
    const long ts = clob::unix_now();
    const HttpHeaders headers = clob::build_l2_headers(cfg_.creds, ts, "DELETE", "/cancel-all", "");
    HttpResponse resp; std::string err;
    if (!http_->request("DELETE", "/cancel-all", "", headers, resp, err)) {
        stats_.last_error = err; return false;
    }
    return resp.status >= 200 && resp.status < 300;
}

bool LiveGateway::adopt_open_orders(std::vector<ManagedOrder>& out) {
    const long ts = clob::unix_now();
    const HttpHeaders headers = clob::build_l2_headers(cfg_.creds, ts, "GET", "/data/orders", "");
    HttpResponse resp; std::string err;
    if (!http_->request("GET", "/data/orders", "", headers, resp, err)) {
        stats_.last_error = err; return false;
    }
    if (resp.status < 200 || resp.status >= 300) { stats_.last_error = resp.body; return false; }

    simdjson::dom::parser p;
    simdjson::dom::element doc;
    if (p.parse(resp.body).get(doc) != simdjson::SUCCESS) return false;
    simdjson::dom::array arr;
    if (doc.get_array().get(arr) != simdjson::SUCCESS) return true;  // empty / not an array

    // Optional string field -> empty view if absent (suppresses warn_unused_result).
    auto opt = [](simdjson::simdjson_result<simdjson::dom::element> f) -> std::string_view {
        std::string_view v;
        return f.get_string().get(v) == simdjson::SUCCESS ? v : std::string_view{};
    };
    for (simdjson::dom::element e : arr) {
        std::string_view asset = opt(e["asset_id"]);
        if (asset.empty()) continue;
        const std::string_view oid     = opt(e["id"]);
        const std::string_view side    = opt(e["side"]);
        const std::string_view sz      = opt(e["original_size"]);
        const std::string_view matched = opt(e["size_matched"]);
        const std::string_view px      = opt(e["price"]);

        interned_.emplace_back(asset);      // stable storage for the token_id view
        ManagedOrder m;
        m.token_id = interned_.back();
        m.side     = (side == "SELL") ? OrderSide::SELL : OrderSide::BUY;
        m.size     = static_cast<Size>(std::strtoul(std::string(sz).c_str(), nullptr, 10));
        m.filled   = static_cast<Size>(std::strtoul(std::string(matched).c_str(), nullptr, 10));
        m.price    = static_cast<Price>(std::strtod(std::string(px).c_str(), nullptr) * 1000.0);
        m.exchange_order_id.assign(oid);
        out.push_back(std::move(m));
    }
    return true;
}

bool LiveGateway::preflight(std::string& reason) {
    // 1) crypto provider correct
    if (!eip712::self_test()) { reason = "keccak self-test FAILED"; return false; }
    // 2) creds + key present
    if (cfg_.creds.api_key.empty() || cfg_.creds.secret.empty() ||
        cfg_.creds.passphrase.empty()) {
        reason = "missing L2 creds (set PM_API_KEY/PM_API_SECRET/PM_API_PASSPHRASE)";
        return false;
    }
    if (!cfg_.priv_loaded) { reason = "missing signer key (set PM_SIGNER_KEY)"; return false; }
    // 3) address(key) == configured signer (lowercased compare)
    const std::string addr = eip712::address_from_privkey(cfg_.priv);
    auto lower = [](std::string s){ for (auto& c : s) if (c>='A'&&c<='Z') c+=32; return s; };
    if (!cfg_.signer.signer.empty() && lower(addr) != lower(cfg_.signer.signer)) {
        reason = "address(PM_SIGNER_KEY) != live_signer_address";
        return false;
    }
    if (lower(cfg_.creds.address) != lower(addr)) {
        reason = "POLY_ADDRESS (creds) != address(key)";
        return false;
    }
    // 4) live order version matches what this binary builds
    HttpResponse resp; std::string err;
    if (!http_->request("GET", "/version", "", {}, resp, err)) {
        reason = "GET /version transport error: " + err; return false;
    }
    long ver = -1;
    {
        simdjson::dom::parser p; simdjson::dom::element doc;
        if (p.parse(resp.body).get(doc) == simdjson::SUCCESS) {
            int64_t v = 0;
            if (doc["version"].get_int64().get(v) == simdjson::SUCCESS) ver = static_cast<long>(v);
        }
    }
    if (ver != cfg_.expected_version) {
        reason = "CLOB /version=" + std::to_string(ver) + " but built for v" +
                 std::to_string(cfg_.expected_version) + " — rebuild before arming";
        return false;
    }
    reason = "ok (signer " + addr + ", /version=" + std::to_string(ver) + ")";
    return true;
}

}  // namespace live
