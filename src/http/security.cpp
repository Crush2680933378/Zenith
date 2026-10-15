#include "zenith/http/security.hpp"

namespace zenith::http {

Middleware corsMiddleware(CorsOptions opt) {
    return [o = std::move(opt)](Context& c) {
        const std::string origin = c.header("Origin");
        // 非浏览器直连（无 Origin）不处理
        if (origin.empty()) {
            c.next();
            return;
        }
        // 白名单语义：allowOrigin 含逗号 → 精确匹配其一
        std::string allow = o.allowOrigin;
        if (o.allowOrigin.find(',') != std::string::npos) {
            allow = "";
            for (const std::string& w : core::string_util::split(o.allowOrigin, ',', false)) {
                if (core::string_util::trim(w) == origin) {
                    allow = origin;
                    break;
                }
            }
            if (allow.empty()) {
                c.status(403).text(403, "origin not allowed");
                return;
            }
        }
        c.setHeader("Access-Control-Allow-Origin", allow);
        if (o.allowCredentials) c.setHeader("Access-Control-Allow-Credentials", "true");
        c.setHeader("Access-Control-Expose-Headers", "X-Request-Id");
        if (c.method() == HttpMethod::Options) {
            c.setHeader("Access-Control-Allow-Methods", o.allowMethods);
            c.setHeader("Access-Control-Allow-Headers", o.allowHeaders);
            c.setHeader("Access-Control-Max-Age", std::to_string(o.maxAgeSec));
            c.status(204).noContent(); // 预检直接答复
            return;
        }
        c.next();
    };
}

TokenBucketLimiter::TokenBucketLimiter(double ratePerSec, double burst)
    : ratePerSec_(ratePerSec), burst_(burst) {}

bool TokenBucketLimiter::allow(const std::string& key) {
    std::lock_guard<std::mutex> lk(mtx_);
    const int64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    Bucket& b = buckets_[key];
    if (b.lastRefillUs == 0) {
        b.tokens = burst_;
        b.lastRefillUs = now;
    }
    // 补充令牌
    const double elapsedSec = static_cast<double>(now - b.lastRefillUs) / 1e6;
    b.tokens += elapsedSec * ratePerSec_;
    if (b.tokens > burst_) b.tokens = burst_;
    b.lastRefillUs = now;

    if (b.tokens >= 1.0) {
        b.tokens -= 1.0;
        return true;
    }
    sweepLocked();
    return false;
}

size_t TokenBucketLimiter::trackedKeys() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return buckets_.size();
}

void TokenBucketLimiter::sweepLocked() {
    // 低频清扫：桶数超 65536 时清空最旧的（简化：全量重建太贵——按容量截断）
    if (buckets_.size() <= 65536) return;
    buckets_.clear();
}

Middleware rateLimitMiddleware(RateLimitOptions opt,
                                std::shared_ptr<TokenBucketLimiter> limiter) {
    auto lim = limiter != nullptr
                   ? std::move(limiter)
                   : std::make_shared<TokenBucketLimiter>(opt.ratePerSec, opt.burst);
    auto retry = opt.retryAfterSec;
    return [lim, retry, keyOf = std::move(opt.keyOf)](Context& c) {
        if (lim->allow(keyOf(c))) {
            c.next();
        } else {
            c.setHeader("Retry-After", std::to_string(retry));
            c.status(429).text(429, "429 too many requests");
        }
    };
}

Middleware securityHeadersMiddleware(bool hsts) {
    return [hsts](Context& c) {
        c.setHeader("X-Content-Type-Options", "nosniff");
        c.setHeader("X-Frame-Options", "DENY");
        c.setHeader("Referrer-Policy", "no-referrer");
        if (hsts) c.setHeader("Strict-Transport-Security", "max-age=31536000; includeSubDomains");
        c.next();
    };
}

Middleware jwtAuthMiddleware(JwtAuthOptions opt) {
    return [o = std::move(opt)](Context& c) {
        // 豁免
        if (!o.exemptPrefixes.empty()) {
            for (const std::string& p : core::string_util::split(o.exemptPrefixes, ',', false)) {
                const std::string pre = core::string_util::trim(p);
                if (!pre.empty() && core::string_util::startsWith(c.path(), pre)) {
                    c.next();
                    return;
                }
            }
        }
        const std::string auth = c.header("Authorization");
        if (!core::string_util::startsWith(auth, "Bearer ")) {
            c.setHeader("WWW-Authenticate", "Bearer realm=\"" + o.realm + "\"");
            c.status(401).text(401, "401 missing bearer token");
            return;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        auto v = jwt::verify(o.secret, auth.substr(7), now);
        if (!v.ok) {
            c.setHeader("WWW-Authenticate", "Bearer realm=\"" + o.realm + "\", error=\"invalid_token\"");
            c.status(401).text(401, "401 " + v.error);
            return;
        }
        c.setContext(std::make_shared<std::string>(std::move(v.payload)));
        c.next();
    };
}

} // namespace zenith::http
