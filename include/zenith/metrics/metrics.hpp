// ============================================================================
//  zenith::metrics —— 指标与可观测（M5.3）
//  - Counter / Gauge（原子）
//  - Histogram：固定桶延迟分布（P50/P90/P99 + Prometheus 导出）
//  - Registry：命名收集 + Prometheus 文本格式输出
//  - 全部线程安全；热路径为原子自增（ns 级）
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::metrics {

class ZENITH_API Counter {
public:
    void inc(int64_t delta = 1) noexcept { value_.fetch_add(delta, std::memory_order_relaxed); }
    [[nodiscard]] int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> value_{0};
};

class ZENITH_API Gauge {
public:
    void set(int64_t v) noexcept { value_.store(v, std::memory_order_relaxed); }
    void inc(int64_t d = 1) noexcept { value_.fetch_add(d, std::memory_order_relaxed); }
    void dec(int64_t d = 1) noexcept { value_.fetch_sub(d, std::memory_order_relaxed); }
    [[nodiscard]] int64_t value() const noexcept {
        return value_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<int64_t> value_{0};
};

/// 固定桶直方图（延迟微秒）：≤1/5/10/50/100/500μs、1/5/10/50/100/500ms、1s+
class ZENITH_API Histogram {
public:
    void observe(int64_t micros) noexcept;
    /// P 分位（0~1）；空样本返回 0
    [[nodiscard]] double percentile(double q) const;
    [[nodiscard]] int64_t count() const noexcept { return count_.load(std::memory_order_relaxed); }
    [[nodiscard]] int64_t sumMicros() const noexcept {
        return sum_.load(std::memory_order_relaxed);
    }
    /// Prometheus 输出（le 桶行 + _sum/_count）
    [[nodiscard]] std::vector<std::string> bucketsForExport(
        const std::string& metricName) const;

    static constexpr size_t kBucketCount = 14;

private:
    std::atomic<int64_t> buckets_[kBucketCount]{};
    std::atomic<int64_t> count_{0};
    std::atomic<int64_t> sum_{0};
};

/// 全局注册表（预置 http_requests / http_latency 等；可自定义）
class ZENITH_API Registry {
public:
    static Registry& instance();

    Counter& counter(const std::string& name);
    Gauge& gauge(const std::string& name);
    Histogram& histogram(const std::string& name);

    // 预置 HTTP 指标（框架侧自动打点）
    Counter& httpRequests() { return counter("zenith_http_requests_total"); }
    Counter& httpErrors() { return counter("zenith_http_errors_total"); }
    Histogram& httpLatency() { return histogram("zenith_http_request_latency_us"); }
    Gauge& httpInflight() { return gauge("zenith_http_inflight"); }

    /// Prometheus 文本格式（# TYPE 行 + 样本行）
    [[nodiscard]] std::string exportPrometheus() const;

    [[nodiscard]] std::vector<std::string> names() const;

private:
    Registry() = default;

    mutable std::mutex mtx_;
    std::map<std::string, std::unique_ptr<Counter>> counters_;
    std::map<std::string, std::unique_ptr<Gauge>> gauges_;
    std::map<std::string, std::unique_ptr<Histogram>> histograms_;
};

/// 进程启动时间（status 用）
ZENITH_API int64_t processUptimeMs() noexcept;

} // namespace zenith::metrics
