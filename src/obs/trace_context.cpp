// ============================================================================
//  W3C Trace Context 实现（M8.7）
// ============================================================================
#include "zenith/obs/trace_context.hpp"

#include <array>
#include <random>

namespace zenith::obs {
namespace {

thread_local std::optional<TraceContext> gThreadTrace;

bool isLowerHex(char c) noexcept {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool isAllHexChars(std::string_view s) noexcept {
    for (char c : s) {
        if (!isLowerHex(c)) return false;
    }
    return true;
}

bool isAllZero(std::string_view s) noexcept {
    for (char c : s) {
        if (c != '0') return false;
    }
    return true;
}

std::string randomHex(size_t digits) {
    // 每个随机 64bit 产出 16 位十六进制
    static const char* kHex = "0123456789abcdef";
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::string out;
    out.reserve(digits);
    while (out.size() < digits) {
        const uint64_t v = rng();
        for (int i = 15; i >= 0 && out.size() < digits; --i) {
            out.push_back(kHex[(v >> (i * 4)) & 0xF]);
        }
    }
    return out;
}

} // namespace

bool TraceContext::valid() const noexcept {
    return traceId.size() == 32 && spanId.size() == 16 && isAllHexChars(traceId) &&
           isAllHexChars(spanId) && !isAllZero(traceId) && !isAllZero(spanId);
}

std::string TraceContext::traceparent() const {
    if (!valid()) return {};
    std::string out;
    out.reserve(55);
    out += "00-";
    out += traceId;
    out += '-';
    out += spanId;
    out += '-';
    out += sampled ? "01" : "00";
    return out;
}

TraceContext TraceContext::child() const {
    TraceContext c;
    c.traceId = traceId;
    c.spanId = randomHex(16);
    c.sampled = sampled;
    return c;
}

TraceContext TraceContext::generate() {
    TraceContext c;
    c.traceId = randomHex(32);
    c.spanId = randomHex(16);
    c.sampled = true;
    return c;
}

std::optional<TraceContext> TraceContext::parse(std::string_view h) {
    // 最少 55 字符：2 + 1 + 32 + 1 + 16 + 1 + 2
    if (h.size() < 55) return std::nullopt;

    const std::string_view version = h.substr(0, 2);
    if (!isAllHexChars(version)) return std::nullopt;
    if (version == "ff") return std::nullopt;       // 规范保留
    if (h[2] != '-' || h[35] != '-' || h[52] != '-') return std::nullopt;

    // version 00 必须是精确 55 字符；更高版本允许附加字段（"-..."）
    if (version == "00" && h.size() != 55) return std::nullopt;

    const std::string_view traceId = h.substr(3, 32);
    const std::string_view spanId = h.substr(36, 16);
    const std::string_view flags = h.substr(53, 2);
    if (!isAllHexChars(traceId) || !isAllHexChars(spanId) || !isAllHexChars(flags)) {
        return std::nullopt;
    }
    if (isAllZero(traceId) || isAllZero(spanId)) return std::nullopt;

    TraceContext c;
    c.traceId.assign(traceId);
    c.spanId.assign(spanId);
    c.sampled = (flags[1] == '1'); // 最低位为 sampled
    return c;
}

TraceContext TraceContext::parseOrGenerate(std::string_view header) {
    auto parsed = parse(header);
    if (parsed.has_value()) return *parsed;
    return generate();
}

std::optional<TraceContext> currentTrace() { return gThreadTrace; }

void setCurrentTrace(std::optional<TraceContext> tc) { gThreadTrace = std::move(tc); }

TraceScope::TraceScope(std::optional<TraceContext> tc) : saved_(gThreadTrace) {
    gThreadTrace = std::move(tc);
}

TraceScope::~TraceScope() { gThreadTrace = std::move(saved_); }

} // namespace zenith::obs
