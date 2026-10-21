#include "zenith/http/jwt.hpp"

#include <cstdlib>

namespace zenith::http::jwt {

std::string sign(const std::string& secret, const std::string& payloadJson,
                 int64_t expiresInSeconds, int64_t nowSec) {
    // exp 注入（简单文本方式：payload 必须是 JSON 对象）
    std::string payload = payloadJson;
    const std::string exp = "\"exp\":" + std::to_string(nowSec + expiresInSeconds);
    if (payload.size() >= 2 && payload.back() == '}') {
        payload.insert(payload.size() - 1, payload.size() > 2 ? ("," + exp) : exp);
    } else {
        payload = "{" + exp + "}";
    }

    const std::string signingInput =
        detail::b64url("{\"alg\":\"HS256\",\"typ\":\"JWT\"}") + "." + detail::b64url(payload);
    uint8_t mac[32];
    crypto::hmacSha256(secret, signingInput, mac);
    return signingInput + "." + detail::b64url(mac, 32);
}

std::optional<int64_t> claimInt(const std::string& payloadJson, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    const size_t p = payloadJson.find(pat);
    if (p == std::string::npos) return std::nullopt;
    const size_t colon = payloadJson.find(':', p + pat.size());
    if (colon == std::string::npos) return std::nullopt;
    return std::strtoll(payloadJson.c_str() + colon + 1, nullptr, 10);
}

VerifyResult verify(const std::string& secret, const std::string& token, int64_t nowSec,
                    int64_t leewaySec) {
    VerifyResult r;
    const size_t d1 = token.find('.');
    const size_t d2 = token.rfind('.');
    if (d1 == std::string::npos || d2 == d1) {
        r.error = "malformed token";
        return r;
    }
    const std::string signingInput = token.substr(0, d2);
    const std::string sigB64 = token.substr(d2 + 1);

    // 重算签名
    uint8_t expect[32];
    crypto::hmacSha256(secret, signingInput, expect);
    const std::string expectB64 = detail::b64url(expect, 32);
    // 恒时比较（编码后等长域）
    if (sigB64.size() != expectB64.size() ||
        !crypto::constantTimeEquals(reinterpret_cast<const uint8_t*>(sigB64.data()),
                                    reinterpret_cast<const uint8_t*>(expectB64.data()),
                                    sigB64.size())) {
        r.error = "bad signature";
        return r;
    }

    auto payload = detail::unb64url(token.substr(d1 + 1, d2 - d1 - 1));
    if (!payload.has_value()) {
        r.error = "bad payload encoding";
        return r;
    }
    auto exp = claimInt(*payload, "exp");
    if (exp.has_value() && nowSec > *exp + leewaySec) {
        r.error = "token expired";
        return r;
    }
    r.ok = true;
    r.payload = std::move(*payload);
    return r;
}

std::string headerAlg(const std::string& token) {
    const size_t d1 = token.find('.');
    if (d1 == std::string::npos) return std::string();
    auto hdr = detail::unb64url(token.substr(0, d1));
    if (!hdr.has_value()) return std::string();
    const size_t key = hdr->find("\"alg\"");
    if (key == std::string::npos) return std::string();
    const size_t colon = hdr->find(':', key + 5);
    if (colon == std::string::npos) return std::string();
    const size_t q1 = hdr->find('"', colon + 1);
    if (q1 == std::string::npos) return std::string();
    const size_t q2 = hdr->find('"', q1 + 1);
    if (q2 == std::string::npos) return std::string();
    return hdr->substr(q1 + 1, q2 - q1 - 1);
}

} // namespace zenith::http::jwt
