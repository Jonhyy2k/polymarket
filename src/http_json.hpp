#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

struct HttpsSessionMetrics {
    uint64_t connect_count = 0;
    uint64_t request_count = 0;
    uint64_t retry_count = 0;
    uint64_t bytes_received = 0;
    double connect_ms_total = 0.0;
    double request_ms_total = 0.0;
    double max_request_ms = 0.0;
};

inline void accumulate_metrics(HttpsSessionMetrics& dst, const HttpsSessionMetrics& src) {
    dst.connect_count += src.connect_count;
    dst.request_count += src.request_count;
    dst.retry_count += src.retry_count;
    dst.bytes_received += src.bytes_received;
    dst.connect_ms_total += src.connect_ms_total;
    dst.request_ms_total += src.request_ms_total;
    if (src.max_request_ms > dst.max_request_ms) {
        dst.max_request_ms = src.max_request_ms;
    }
}

class HttpsSession {
public:
    explicit HttpsSession(std::string host, std::string port = "443");
    ~HttpsSession();

    bool get(std::string_view target, std::string& body, std::string& error,
             double* request_ms = nullptr);

    const HttpsSessionMetrics& metrics() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
