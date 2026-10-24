// ============================================================================
//  OtelExporter 实现 —— OTLP HTTP/JSON（M13 第四批）
// ============================================================================
#include "zenith/http/otel.hpp"

#include <chrono>
#include <cstdio>
#include <random>

#include "zenith/ai/provider_router.hpp"
#include "zenith/json/json.hpp"

namespace zenith::obs {
namespace {

std::string hexRandom(std::mt19937_64& rng, size_t nibbles) {
    static const char* kHex = "0123456789abcdef";
    std::string out(nibbles, '0');
    for (size_t i = 0; i < nibbles; ++i) {
        out[i] = kHex[rng() & 0xF];
    }
    return out;
}

std::mt19937_64& rng() {
    static thread_local std::mt19937_64 r{std::random_device{}()};
    return r;
}

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

} // namespace

OtelExporter::OtelExporter(OtelConfig cfg) : cfg_(std::move(cfg)) {
    if (cfg_.flushIntervalMs > 0) {
        running_.store(true);
        thread_ = std::thread([this] { run(); });
    }
}

OtelExporter::~OtelExporter() { stop(); }

OtelSpan OtelExporter::beginSpan(const std::string& name,
                                 const std::string& parentTraceId,
                                 const std::string& parentSpanId) const {
    OtelSpan s;
    s.traceId = parentTraceId.empty() ? hexRandom(rng(), 32) : parentTraceId;
    s.spanId = hexRandom(rng(), 16);
    s.parentSpanId = parentSpanId;
    s.name = name;
    s.startMs = nowMs();
    return s;
}

void OtelExporter::endSpan(OtelSpan& s,
                           std::vector<std::pair<std::string, std::string>> attrs) {
    if (s.endMs == 0) s.endMs = nowMs();
    for (auto& a : attrs) s.attributes.push_back(std::move(a));
    std::lock_guard<std::mutex> lk(mtx_);
    queue_.push_back(std::move(s));
}

size_t OtelExporter::pending() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return queue_.size();
}

size_t OtelExporter::flushLocked() {
    if (queue_.empty() || cfg_.endpoint.empty()) return 0;
    std::vector<OtelSpan> batch;
    batch.reserve(std::min(queue_.size(), cfg_.maxBatch));
    for (size_t i = 0; i < cfg_.maxBatch && !queue_.empty(); ++i) {
        batch.push_back(std::move(queue_.back()));
        queue_.pop_back();
    }

    // OTLP JSON（resourceSpans → scopeSpans → spans）
    using json::JsonValue;
    JsonValue spans = JsonValue::array();
    for (const OtelSpan& s : batch) {
        JsonValue j = JsonValue::object();
        j["traceId"] = s.traceId;
        j["spanId"] = s.spanId;
        if (!s.parentSpanId.empty()) j["parentSpanId"] = s.parentSpanId;
        j["name"] = s.name;
        j["kind"] = int64_t{1}; // INTERNAL
        j["startTimeUnixNano"] = std::to_string(s.startMs * 1000000);
        j["endTimeUnixNano"] = std::to_string(s.endMs * 1000000);
        JsonValue attrs = JsonValue::array();
        for (const auto& [k, v] : s.attributes) {
            JsonValue a = JsonValue::object();
            a["key"] = k;
            a["value"]["stringValue"] = v;
            attrs.push_back(std::move(a));
        }
        j["attributes"] = std::move(attrs);
        spans.push_back(std::move(j));
    }
    JsonValue scopeSpans = JsonValue::object();
    scopeSpans["scope"]["name"] = "zenith.obs";
    scopeSpans["spans"] = std::move(spans);
    JsonValue arr1 = JsonValue::array();
    arr1.push_back(std::move(scopeSpans));

    JsonValue res = JsonValue::object();
    JsonValue resAttrs = JsonValue::array();
    JsonValue sa = JsonValue::object();
    sa["key"] = "service.name";
    sa["value"]["stringValue"] = cfg_.serviceName;
    resAttrs.push_back(std::move(sa));
    res["attributes"] = std::move(resAttrs);

    JsonValue rs = JsonValue::object();
    rs["resource"] = std::move(res);
    rs["scopeSpans"] = std::move(arr1);
    JsonValue arr0 = JsonValue::array();
    arr0.push_back(std::move(rs));
    JsonValue root = JsonValue::object();
    root["resourceSpans"] = std::move(arr0);

    ai::SimpleHttpCall call;
    call.method = "POST";
    call.url = cfg_.endpoint;
    call.body = root.dump();
    call.headers = {{"Content-Type", "application/json"}};
    for (const auto& [k, v] : cfg_.headers) call.headers.emplace_back(k, v);
    auto r = ai::simpleHttpRequest(call);
    if (r.ok) {
        exported_.fetch_add(batch.size());
        return batch.size();
    }
    // 失败回队（保序无所谓，顺序只影响展示）
    for (OtelSpan& s : batch) queue_.push_back(std::move(s));
    failed_.fetch_add(1);
    return 0;
}

size_t OtelExporter::flushNow() {
    std::lock_guard<std::mutex> lk(mtx_);
    return flushLocked();
}

void OtelExporter::run() {
    while (running_.load()) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg_.flushIntervalMs > 0 ? cfg_.flushIntervalMs : 1000));
        std::lock_guard<std::mutex> lk(mtx_);
        while (!queue_.empty()) {
            if (flushLocked() == 0) break; // 失败即退（下轮再试）
        }
    }
}

void OtelExporter::stop() {
    if (running_.exchange(false)) {
        if (thread_.joinable()) thread_.join();
    }
    std::lock_guard<std::mutex> lk(mtx_);
    while (!queue_.empty()) {
        if (flushLocked() == 0) break; // 端点不可达则丢弃余量
    }
}

} // namespace zenith::obs
