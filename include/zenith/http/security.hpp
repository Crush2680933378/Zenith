// ============================================================================
//  zenith::http::security —— 安全中间件族（M5.2）
//  CORS / 令牌桶限流 / 安全响应头 / JWT 鉴权
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include "zenith/core/export.hpp"
#include "zenith/core/string.hpp"
#include "zenith/http/context.hpp"
#include "zenith/http/jwt.hpp"

namespace zenith::http {

// ---------------------------------------------------------------------------
// CORS
// ---------------------------------------------------------------------------
struct CorsOptions {
    std::string allowOrigin = "*";           // 或逗号分隔白名单精确匹配
    bool allowCredentials = false;
    std::string allowMethods = "GET, POST, PUT, DELETE, OPTIONS";
    std::string allowHeaders = "Content-Type, Authorization, X-Request-Id";
    int maxAgeSec = 86400;
};

/// CORS 中间件：预检 OPTIONS 直接 204；实际请求补头
ZENITH_API Middleware corsMiddleware(CorsOptions opt = {});

// ---------------------------------------------------------------------------
// 令牌桶限流
// ---------------------------------------------------------------------------
struct RateLimitOptions {
    double ratePerSec = 100;      // 令牌生成速率
    double burst = 100;           // 桶容量
    int64_t retryAfterSec = 1;    // 429 附带 Retry-After
    /// 键提取（默认 IP）
    std::function<std::string(Context&)> keyOf = [](Context& c) {
        const size_t colon = c.peer().rfind(':');
        return colon == std::string::npos ? c.peer() : c.peer().substr(0, colon);
    };
};

class ZENITH_API TokenBucketLimiter {
public:
    TokenBucketLimiter(double ratePerSec, double burst);
    /// 取一枚令牌；返回 false = 超限
    bool allow(const std::string& key);
    [[nodiscard]] size_t trackedKeys() const;

private:
    void sweepLocked();

    struct Bucket {
        double tokens;
        int64_t lastRefillUs;
    };
    mutable std::mutex mtx_;  // const 方法(trackedKeys)亦需读取
    std::unordered_map<std::string, Bucket> buckets_;
    double ratePerSec_;
    double burst_;
};

/// 限流中间件
ZENITH_API Middleware rateLimitMiddleware(RateLimitOptions opt = {},
                                          std::shared_ptr<TokenBucketLimiter> limiter = nullptr);

// ---------------------------------------------------------------------------
// 安全响应头
// ---------------------------------------------------------------------------
/// 一键安全头（HSTS 仅 https 场景建议开启）
ZENITH_API Middleware securityHeadersMiddleware(bool hsts = false);

// ---------------------------------------------------------------------------
// JWT 鉴权
// ---------------------------------------------------------------------------
struct JwtAuthOptions {
    std::string secret;
    std::string realm = "zenith";
    /// 豁免路径前缀（如 "/login,/public"）；空 = 全部校验
    std::string exemptPrefixes;
};

/// JWT 中间件：Authorization: Bearer <token> → 校验 → payload 挂 context
/// （contextAs<std::string>() 取 payload JSON）；失败 401 + WWW-Authenticate
ZENITH_API Middleware jwtAuthMiddleware(JwtAuthOptions opt);

} // namespace zenith::http
