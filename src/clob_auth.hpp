#pragma once

// ┌────────────────────────────────────────────────────────────────────────────┐
// │ Polymarket CLOB L2 (HMAC) request authentication.                          │
// │                                                                            │
// │ Builds the five POLY_* headers every authenticated REST call needs. This   │
// │ is KEYLESS with respect to the EOA private key: it uses the API *secret*    │
// │ (returned by L1 key-derivation, POST /auth/api-key) — NOT the wallet key.   │
// │ Building this header signs no order and cannot move funds. The L1 step that │
// │ produces these creds DOES require an EOA signature and is gated elsewhere.  │
// │                                                                            │
// │ Scheme (matches py-clob-client build_hmac_signature):                      │
// │   secret_bytes = base64url_decode(secret)                                   │
// │   message      = str(timestamp) + METHOD + requestPath + body              │
// │   signature    = base64url_encode( HMAC_SHA256(secret_bytes, message) )     │
// │   headers      = POLY_ADDRESS / POLY_SIGNATURE / POLY_TIMESTAMP /           │
// │                  POLY_API_KEY / POLY_PASSPHRASE                            │
// │                                                                            │
// │ CONTRACT: the `body` passed here MUST be byte-identical to the body sent on │
// │ the wire — the HMAC covers the exact bytes. Verify against a captured live  │
// │ request before arming.                                                     │
// └────────────────────────────────────────────────────────────────────────────┘

#include <openssl/hmac.h>
#include <openssl/evp.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <string_view>
#include <vector>

#include "http_json.hpp"   // HttpHeaders

namespace clob {

// API credentials from L1 derivation. `secret` is the base64url string exactly
// as returned by POST /auth/api-key.
struct L2Creds {
    std::string api_key;
    std::string secret;      // base64url
    std::string passphrase;
    std::string address;     // signer/funder address -> POLY_ADDRESS
};

namespace detail {

inline std::string b64_encode(const unsigned char* data, size_t len, bool url) {
    static const char STD[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static const char URL[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* T = url ? URL : STD;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back(T[n & 63]);
    }
    const size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = uint32_t(data[i]) << 16;
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back('=');
        out.push_back('=');
    } else if (rem == 2) {
        uint32_t n = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
        out.push_back(T[(n >> 18) & 63]);
        out.push_back(T[(n >> 12) & 63]);
        out.push_back(T[(n >> 6) & 63]);
        out.push_back('=');
    }
    return out;
}

inline int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;   // accept both std and url-safe
    if (c == '/' || c == '_') return 63;
    return -1;                              // '=' padding / whitespace / invalid
}

// Tolerant base64 decode: accepts both standard and url-safe alphabets and
// missing padding (skips any non-alphabet byte, stops nothing early).
inline std::vector<unsigned char> b64_decode(std::string_view s) {
    std::vector<unsigned char> out;
    out.reserve(s.size() * 3 / 4 + 3);
    int buf = 0, bits = 0;
    for (char c : s) {
        if (c == '=') break;
        int v = b64_val(c);
        if (v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buf >> bits) & 0xff));
        }
    }
    return out;
}

}  // namespace detail

// HMAC-SHA256(base64url-decoded secret, message) -> base64url signature.
inline std::string hmac_sha256_b64url(std::string_view secret_b64url,
                                      std::string_view message) {
    const std::vector<unsigned char> key = detail::b64_decode(secret_b64url);
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int  mac_len = 0;
    HMAC(EVP_sha256(),
         key.empty() ? reinterpret_cast<const unsigned char*>("") : key.data(),
         static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(message.data()), message.size(),
         mac, &mac_len);
    return detail::b64_encode(mac, mac_len, /*url=*/true);
}

// The signed pre-image per Polymarket L2: timestamp + METHOD + path + body.
inline std::string l2_message(long timestamp, std::string_view method,
                              std::string_view path, std::string_view body) {
    std::string m;
    m.reserve(24 + method.size() + path.size() + body.size());
    m += std::to_string(timestamp);
    m.append(method);
    m.append(path);
    m.append(body);
    return m;
}

// Build the five POLY_* headers for one authenticated request.
inline HttpHeaders build_l2_headers(const L2Creds& c, long timestamp,
                                    std::string_view method, std::string_view path,
                                    std::string_view body) {
    const std::string sig =
        hmac_sha256_b64url(c.secret, l2_message(timestamp, method, path, body));
    return HttpHeaders{
        {"POLY_ADDRESS",    c.address},
        {"POLY_SIGNATURE",  sig},
        {"POLY_TIMESTAMP",  std::to_string(timestamp)},
        {"POLY_API_KEY",    c.api_key},
        {"POLY_PASSPHRASE", c.passphrase},
    };
}

inline long unix_now() noexcept { return static_cast<long>(std::time(nullptr)); }

}  // namespace clob
