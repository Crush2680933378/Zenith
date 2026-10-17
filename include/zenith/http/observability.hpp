// ============================================================================
//  zenith::http::observability —— 指标/追踪/状态三件中间件（M5.3）
//  - metricsMiddleware：QPS/错误/在途/延迟直方图自动打点
//  - requestIdMiddleware：X-Request-Id 生成或透传（响应回写、日志字段）
//  - prometheusHandler()：GET /metrics 导出
//  - statusHandler()：GET /zenith/status 运行时 JSON
// ============================================================================
#pragma once

#include <atomic>
#include <random>
#include <string>

#include "zenith/core/time_util.hpp"
#include "zenith/http/context.hpp"
#include "zenith/metrics/metrics.hpp"

namespace zenith::http {

/// 指标中间件（全局 use）
inline Middleware metricsMiddleware(metrics::Registry& reg = metrics::Registry::instance()) {
    return [&reg](Context& c) {
        reg.httpRequests().inc();
        reg.httpInflight().inc();
        const int64_t t0 = core::steadyMicroseconds();
        try {
            c.next();
        } catch (...) {
            // 异常向上交给服务器兜底 500——先记录再重抛（否则指标漏记）
            reg.httpLatency().observe(core::steadyMicroseconds() - t0);
            reg.httpInflight().dec();
            reg.httpErrors().inc();
            throw;
        }
        reg.httpLatency().observe(core::steadyMicroseconds() - t0);
        reg.httpInflight().dec();
        if (c.responseStatus() >= 500) reg.httpErrors().inc();
    };
}

/// Request-Id 中间件：入站无则生成（32 hex），响应回写
inline Middleware requestIdMiddleware() {
    return [](Context& c) {
        std::string id = c.header("X-Request-Id");
        if (id.empty() || id.size() > 64) {
            static thread_local std::mt19937_64 rng{std::random_device{}()};
            static const char* kHex = "0123456789abcdef";
            id.reserve(32);
            for (int i = 0; i < 16; ++i) {
                const uint64_t v = rng();
                id.push_back(kHex[(v >> 4) & 0xF]);
                id.push_back(kHex[v & 0xF]);
            }
        }
        c.setHeader("X-Request-Id", id);
        c.setContext(std::make_shared<std::string>(id)); // contextAs<std::string>()
        c.next();
    };
}

/// /metrics 处理器
inline Handler prometheusHandler(metrics::Registry& reg = metrics::Registry::instance()) {
    return [&reg](Context& c) {
        c.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        c.status(200);
        c.response().body = reg.exportPrometheus();
    };
}

/// /zenith/status 处理器（运行时状态 JSON；可挂额外 gauge 供给连接/fd 等）
inline Handler statusHandler(
    std::function<int64_t(const std::string& key)> extraGauge = nullptr) {
    return [extraGauge](Context& c) {
        auto& reg = metrics::Registry::instance();
        json::JsonValue v = json::JsonValue::object();
        v["uptime_ms"] = static_cast<int64_t>(metrics::processUptimeMs());
        v["http_requests_total"] = reg.httpRequests().value();
        v["http_errors_total"] = reg.httpErrors().value();
        v["http_inflight"] = reg.httpInflight().value();
        if (extraGauge) {
            if (auto conns = extraGauge("connections")) v["connections"] = conns;
            if (auto poolHit = extraGauge("pool_hit_rate_permille")) {
                v["pool_hit_rate_permille"] = poolHit; // 0~1000（千分比）
            }
        }
        json::JsonValue lat = json::JsonValue::object();
        lat["p50_sec"] = reg.httpLatency().percentile(0.50);
        lat["p99_sec"] = reg.httpLatency().percentile(0.99);
        lat["count"] = static_cast<int64_t>(reg.httpLatency().count());
        v["latency"] = std::move(lat);
        c.json(200, v);
    };
}

} // namespace zenith::http
