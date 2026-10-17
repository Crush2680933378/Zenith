#include "zenith/metrics/metrics.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace zenith::metrics {

namespace {
constexpr int64_t kBucketBoundsUs[Histogram::kBucketCount] = {
    1, 5, 10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 1000000,
    9223372036854775807LL /* +Inf */
};
const char* kBucketLabels[Histogram::kBucketCount] = {
    "0.000001", "0.000005", "0.00001", "0.00005", "0.0001", "0.0005", "0.001",
    "0.005", "0.01", "0.05", "0.1", "0.5", "1.0", "+Inf"};

const int64_t kProcessStartMs = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}();
} // namespace

void Histogram::observe(int64_t micros) noexcept {
    sum_.fetch_add(micros, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    for (size_t i = 0; i < kBucketCount; ++i) {
        if (micros <= kBucketBoundsUs[i]) {
            buckets_[i].fetch_add(1, std::memory_order_relaxed);
        }
    }
}

double Histogram::percentile(double q) const {
    const int64_t n = count_.load(std::memory_order_relaxed);
    if (n == 0) return 0.0;
    const double target = static_cast<double>(n) * q;
    int64_t cumulative = 0;
    for (size_t i = 0; i < kBucketCount; ++i) {
        cumulative = buckets_[i].load(std::memory_order_relaxed);
        if (static_cast<double>(cumulative) >= target) {
            // 返回该桶的界（秒）——粗粒度但稳定
            const double boundSec =
                i + 1 < kBucketCount
                    ? static_cast<double>(kBucketBoundsUs[i]) / 1e6
                    : 1.0;
            return boundSec;
        }
    }
    return 1.0;
}

std::vector<std::string> Histogram::bucketsForExport(const std::string& name) const {
    std::vector<std::string> out;
    out.reserve(kBucketCount + 2);
    for (size_t i = 0; i < kBucketCount; ++i) {
        char line[160];
        std::snprintf(line, sizeof(line), "%s_bucket{le=\"%s\"} %lld", name.c_str(),
                      kBucketLabels[i],
                      static_cast<long long>(buckets_[i].load(std::memory_order_relaxed)));
        out.push_back(line);
    }
    char tail[2][96];
    std::snprintf(tail[0], sizeof(tail[0]), "%s_sum %lld", name.c_str(),
                  static_cast<long long>(sum_.load(std::memory_order_relaxed)));
    std::snprintf(tail[1], sizeof(tail[1]), "%s_count %lld", name.c_str(),
                  static_cast<long long>(count_.load(std::memory_order_relaxed)));
    out.push_back(tail[0]);
    out.push_back(tail[1]);
    return out;
}

Registry& Registry::instance() {
    static Registry r;
    return r;
}

Counter& Registry::counter(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& p = counters_[name];
    if (!p) p = std::make_unique<Counter>();
    return *p;
}

Gauge& Registry::gauge(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& p = gauges_[name];
    if (!p) p = std::make_unique<Gauge>();
    return *p;
}

Histogram& Registry::histogram(const std::string& name) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto& p = histograms_[name];
    if (!p) p = std::make_unique<Histogram>();
    return *p;
}

std::vector<std::string> Registry::names() const {
    std::vector<std::string> out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& [n, _] : counters_) out.push_back("c:" + n);
    for (const auto& [n, _] : gauges_) out.push_back("g:" + n);
    for (const auto& [n, _] : histograms_) out.push_back("h:" + n);
    return out;
}

std::string Registry::exportPrometheus() const {
    std::string out;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const auto& [n, c] : counters_) {
        out += "# TYPE " + n + " counter\n" + n + " " + std::to_string(c->value()) + "\n";
    }
    for (const auto& [n, g] : gauges_) {
        out += "# TYPE " + n + " gauge\n" + n + " " + std::to_string(g->value()) + "\n";
    }
    for (const auto& [n, h] : histograms_) {
        out += "# TYPE " + n + " histogram\n";
        for (const std::string& l : h->bucketsForExport(n)) {
            out += l;
            out += '\n';
        }
    }
    return out;
}

int64_t processUptimeMs() noexcept {
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
    return now - kProcessStartMs;
}

} // namespace zenith::metrics
