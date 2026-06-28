#pragma once

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ RelayGateway — IExecGateway that forwards order intents to the local Python │
// │ order-gateway service (tools/order_gateway_server.py), which signs sigType-3 │
// │ (POLY_1271 / ERC-7739 TypedDataSign — which the C++ signer can't yet do) and │
// │ POSTs to the Polymarket CLOB. The fast strategy/OMS stays in C++; only the   │
// │ deposit-wallet signing lives in Python.                                      │
// │                                                                            │
// │ Transport: plain HTTP/1.1 over a fresh loopback TCP socket per request       │
// │ (127.0.0.1 only). Bearer-token auth. Runs on the single cancel-sender thread │
// │ so the client_id→order_id map needs no lock.                                 │
// │                                                                            │
// │ SAFETY: arm == false (default) builds the request but does NOT send — a dry  │
// │ run. The auth token is provided by the caller (env), never logged.           │
// └────────────────────────────────────────────────────────────────────────────┘

#include "oms.hpp"   // IExecGateway, ManagedOrder, OrderSide, PRICE_ONE

#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace live {

struct RelayConfig {
    std::string host  = "127.0.0.1";
    int         port  = 8765;
    std::string token;            // bearer auth (from $ORDER_GW_TOKEN)
    bool        arm   = false;    // false => build but DO NOT send (dry run)
    bool        verbose = false;
};

class RelayGateway : public IExecGateway {
public:
    explicit RelayGateway(RelayConfig cfg) : cfg_(std::move(cfg)) {}

    // GET /health — confirms the connector is up and reports the funder/signer.
    bool preflight(std::string& err) {
        HttpResult r;
        if (!http("GET", "/health", "", r)) { err = "relay: connector unreachable (" + r.err + ")"; return false; }
        if (r.status != 200 || r.body.find("\"ok\": true") == std::string::npos) {
            err = "relay: /health bad: " + std::to_string(r.status) + " " + r.body; return false;
        }
        if (cfg_.verbose) std::printf("[RELAY] preflight OK: %s\n", r.body.c_str());
        return true;
    }

    bool submit(const ManagedOrder& o) override {
        const std::string body = place_body(o);
        if (!cfg_.arm) {
            if (cfg_.verbose) std::printf("[RELAY dry] place %s\n", body.c_str());
            ++dry_; return true;
        }
        HttpResult r;
        if (!http("POST", "/place", body, r) || r.status != 200) {
            std::printf("[RELAY] place FAILED (%d): %s\n", r.status, (r.status ? r.body : r.err).c_str());
            return false;
        }
        if (r.body.find("\"success\": true") == std::string::npos &&
            r.body.find("\"success\":true") == std::string::npos) {
            std::printf("[RELAY] place rejected: %s\n", r.body.c_str());
            return false;
        }
        std::string oid = json_str(r.body, "order_id");
        if (!oid.empty()) id_map_[o.client_id] = oid;
        ++sent_;
        if (cfg_.verbose) std::printf("[RELAY] placed cid=%llu -> %s\n",
                                      (unsigned long long)o.client_id, oid.c_str());
        return true;
    }

    bool cancel(const ManagedOrder& o) override {
        auto it = id_map_.find(o.client_id);
        if (it == id_map_.end()) return true;   // never acked => nothing resting
        if (!cfg_.arm) { if (cfg_.verbose) std::printf("[RELAY dry] cancel %s\n", it->second.c_str()); return true; }
        const std::string body = "{\"order_id\":\"" + it->second + "\"}";
        HttpResult r;
        bool ok = http("POST", "/cancel", body, r) && r.status == 200;
        if (ok) id_map_.erase(it);
        else std::printf("[RELAY] cancel FAILED (%d): %s\n", r.status, (r.status ? r.body : r.err).c_str());
        return ok;
    }

    // Flatten everything resting at the exchange (startup + shutdown).
    bool cancel_all() {
        if (!cfg_.arm) return true;
        HttpResult r;
        bool ok = http("POST", "/cancel_all", "{}", r) && r.status == 200;
        id_map_.clear();
        if (cfg_.verbose) std::printf("[RELAY] cancel_all -> %d %s\n", r.status, r.body.c_str());
        return ok;
    }

    uint64_t sent() const noexcept { return sent_; }
    uint64_t dry()  const noexcept { return dry_; }

private:
    struct HttpResult { int status = 0; std::string body; std::string err; };

    std::string place_body(const ManagedOrder& o) const {
        char price[16];
        std::snprintf(price, sizeof(price), "%.3f",
                      static_cast<double>(o.price) / static_cast<double>(PRICE_ONE));
        std::string b = "{\"token_id\":\"";
        b.append(o.token_id.data(), o.token_id.size());
        b += "\",\"side\":\"";
        b += (o.side == OrderSide::BUY ? "BUY" : "SELL");
        b += "\",\"price\":";  b += price;
        b += ",\"size\":";     b += std::to_string(o.size);
        b += ",\"post_only\":true,\"neg_risk\":";
        b += (o.neg_risk ? "true" : "false");
        b += "}";
        return b;
    }

    // Extract a JSON string value:  "key":"<value>"  (loopback connector output only).
    static std::string json_str(const std::string& body, const char* key) {
        std::string k = "\"" + std::string(key) + "\"";
        auto p = body.find(k);
        if (p == std::string::npos) return {};
        p = body.find(':', p + k.size());
        if (p == std::string::npos) return {};
        while (p < body.size() && (body[p] == ':' || body[p] == ' ')) ++p;
        if (p >= body.size() || body[p] != '"') return {};
        auto e = body.find('"', ++p);
        return e == std::string::npos ? std::string{} : body.substr(p, e - p);
    }

    bool http(const char* method, const char* path, const std::string& body, HttpResult& out) {
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { out.err = "socket"; return false; }
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(cfg_.port);
        if (::inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) { out.err = "inet_pton"; ::close(fd); return false; }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { out.err = "connect"; ::close(fd); return false; }
        int one = 1; ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        std::string req = std::string(method) + " " + path + " HTTP/1.1\r\n";
        req += "Host: " + cfg_.host + "\r\n";
        if (!cfg_.token.empty()) req += "Authorization: Bearer " + cfg_.token + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n\r\n";
        req += body;
        for (size_t off = 0; off < req.size(); ) {
            ssize_t n = ::send(fd, req.data() + off, req.size() - off, 0);
            if (n <= 0) { out.err = "send"; ::close(fd); return false; }
            off += static_cast<size_t>(n);
        }
        std::string resp; char buf[4096]; ssize_t n;
        while ((n = ::recv(fd, buf, sizeof(buf), 0)) > 0) resp.append(buf, static_cast<size_t>(n));
        ::close(fd);
        auto sp = resp.find(' ');
        if (sp != std::string::npos) out.status = std::atoi(resp.c_str() + sp + 1);
        auto hdr_end = resp.find("\r\n\r\n");
        out.body = (hdr_end == std::string::npos) ? "" : resp.substr(hdr_end + 4);
        return out.status != 0;
    }

    RelayConfig cfg_;
    std::unordered_map<uint64_t, std::string> id_map_;   // client_id -> exchange order id
    uint64_t sent_ = 0, dry_ = 0;
};

}  // namespace live
