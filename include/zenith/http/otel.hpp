// ============================================================================
//  zenith::obs::OtelExporter —— OpenTelemetry OTLP/HTTP-JSON 导出（M13 第四批）
//
//  自研最小实现：Span 生成（W3C 128bit traceId / 64bit spanId 十六进制）→
//  内存队列 → 批量 POST {endpoint}（OTLP JSON：resourceSpans/scopeSpans）。
//  flushIntervalMs>0 启动后台定时刷；=0 仅手动 flushNow()（测试/同步场景）。
//  传输复用 ai::simpleHttpRequest（http/https 均可，TLS 走 OpenSSL 后端）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::obs {

struct ZENITH_API OtelConfig {
    std::string endpoint; // OTLP traces 端点，如 http://otel:4318/v1/traces
    std::string serviceName = "zenith";
    std::vector<std::pair<std::string, std::string>> headers; // 如 Authorization
    int flushIntervalMs = 2000; // 0 = 不启后台线程
    size_t maxBatch = 64;       // 单次导出上限（余量留下批）
};

struct ZENITH_API OtelSpan {
    std::string traceId;      // 32 hex
    std::string spanId;       // 16 hex
    std::string parentSpanId; // 可空
    std::string name;
    int64_t startMs = 0;
    int64_t endMs = 0;
    std::vector<std::pair<std::string, std::string>> attributes;

    [[nodiscard]] bool valid() const {
        return !traceId.empty() && !spanId.empty() && endMs >= startMs;
    }
};

class ZENITH_API OtelExporter {
public:
    explicit OtelExporter(OtelConfig cfg);
    ~OtelExporter();
    OtelExporter(const OtelExporter&) = delete;
    OtelExporter& operator=(const OtelExporter&) = delete;

    /// 开启并返回 span（start=now）；结束调 endSpan
    [[nodiscard]] OtelSpan beginSpan(const std::string& name,
                                     const std::string& parentTraceId = {},
                                     const std::string& parentSpanId = {}) const;
    /// 填 endMs + 属性并入队
    void endSpan(OtelSpan& s,
                 std::vector<std::pair<std::string, std::string>> attrs = {});

    /// 同步导出一批（测试/关停前调用）；返回本次送出的 span 数
    size_t flushNow();

    [[nodiscard]] uint64_t exportedSpans() const { return exported_.load(); }
    [[nodiscard]] uint64_t failedFlushes() const { return failed_.load(); }
    [[nodiscard]] size_t pending() const;
    void stop();

private:
    void run();
    size_t sendBatch(const std::vector<OtelSpan>& batch); // 锁外上传

    OtelConfig cfg_;
    mutable std::mutex mtx_;
    std::vector<OtelSpan> queue_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::atomic<uint64_t> exported_{0};
    std::atomic<uint64_t> failed_{0};
};

} // namespace zenith::obs
