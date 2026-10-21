// ============================================================================
//  zenith::http::jwt —— JWT 签发/校验（M5.2 HS256，M9.2 非对称族）
//  - HS256：header.payload.signature（base64url）+ HMAC-SHA256 + 恒时比较
//    payload 为调用方 JSON 字符串（通常 zenith::json 序列化产物）
//  - RS256/384/512、ES256/384：走 OpenSSL（构建期 ZENITH_HAS_OPENSSL）
//    对接第三方 IdP（GitHub/Google/Apple/微信）与 Passkey/ACME 签名场景
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/crypto/base64.hpp"
#include "zenith/crypto/sha256.hpp"

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

/// 提取 token header 中的 alg（不做校验，仅用于选路）；缺失返回空串
ZENITH_API std::string headerAlg(const std::string& token);

#if defined(ZENITH_HAS_OPENSSL)
// ---------------------------------------------------------------------------
//  非对称签名族（M9.2）—— 仅 OpenSSL 后端可用
//  RSA 私钥 → RS256/RS384/RS512；P-256/P-384 EC 私钥 → ES256/ES384
// ---------------------------------------------------------------------------
namespace asym {

struct KeyPair {
    std::string privatePem; // PKCS#8 PEM
    std::string publicPem;  // SubjectPublicKeyInfo PEM
};

/// 生成 RSA 密钥对（默认 2048 位）
ZENITH_API core::Result<KeyPair> generateRsa(int bits = 2048);
/// 生成 EC 密钥对（P-256，配 ES256）
ZENITH_API core::Result<KeyPair> generateEcP256();

/// 签发：算法由私钥类型决定（RSA→RS256，P-256→ES256，P-384→ES384）。
ZENITH_API core::Result<std::string> sign(const std::string& privatePem,
                                          const std::string& payloadJson,
                                          int64_t expiresInSeconds, int64_t nowSec);
ZENITH_API core::Result<std::string> sign(const KeyPair& kp, const std::string& payloadJson,
                                          int64_t expiresInSeconds, int64_t nowSec);

/// 校验：从 token header 取 alg，只接受非对称算法（RS*/ES*/PS*），
/// 显式拒绝 none 与 HS*（防算法混淆攻击）；再校验 exp。
ZENITH_API VerifyResult verify(const std::string& publicPem, const std::string& token,
                               int64_t nowSec, int64_t leewaySec = 30);

} // namespace asym
#endif // ZENITH_HAS_OPENSSL

} // namespace zenith::http::jwt
