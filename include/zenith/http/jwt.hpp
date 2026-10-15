// ============================================================================
//  zenith::http::jwt —— JWT 签发/校验（HS256，M5.2）
//  header.payload.signature（base64url）+ HMAC-SHA256 + 恒时比较
//  payload 为调用方 JSON 字符串（通常 zenith::json 序列化产物）
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "zenith/crypto/base64.hpp"
#include "zenith/crypto/sha256.hpp"
#include "zenith/core/export.hpp"

namespace zenith::http::jwt {

namespace detail {
inline std::string b64url(const uint8_t* d, size_t n) {
    std::string s = crypto::base64Encode(d, n);
    // 标准表 → URL 安全表；去填充
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}
inline std::string b64url(const std::string& s) {
    return b64url(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}
inline std::optional<std::string> unb64url(const std::string& s) {
    std::string t = s;
    for (char& c : t) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    switch (t.size() % 4) {
        case 2: t += "=="; break;
        case 3: t += "="; break;
        case 0: break;
        default: return std::nullopt;
    }
    return crypto::base64Decode(t);
}
} // namespace detail

/// 签发：payloadJson + 秒级有效期 → token
ZENITH_API std::string sign(const std::string& secret, const std::string& payloadJson,
                            int64_t expiresInSeconds, int64_t nowSec);

struct VerifyResult {
    bool ok = false;
    std::string payload;  // ok 时有效
    std::string error;    // 失败原因
};

/// 校验：签名（恒时比较）+ exp 过期。leewaySec 容忍时钟偏差。
ZENITH_API VerifyResult verify(const std::string& secret, const std::string& token,
                               int64_t nowSec, int64_t leewaySec = 30);

/// 提取 payload 中的数值字段（如 exp）；无返回 nullopt
ZENITH_API std::optional<int64_t> claimInt(const std::string& payloadJson,
                                            const std::string& key);

} // namespace zenith::http::jwt
