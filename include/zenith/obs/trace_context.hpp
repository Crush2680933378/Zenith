// ============================================================================
//  zenith::obs::TraceContext —— W3C Trace Context 跨服务链路传播（M8.7）
//
//  解决的问题：单进程内的 OTel span 已经能串起来，但一次请求穿过
//  A → B → C 三个服务时，各自的 trace 是断的。W3C 标准用 `traceparent`
//  头把「同一条链路」传递下去：
//      traceparent: 00-<32hex trace-id>-<16hex parent-id>-<2hex flags>
//
//  用法：
//    - 服务端：HttpServer 在每个请求派发前解析入站 traceparent，写入线程
//      上下文（TraceScope）；离开作用域自动恢复。
//    - 客户端：HttpClient 序列化请求时若线程上下文有效，自动补 traceparent。
//    两边都不需要业务代码参与，跨服务链路自动串起来。
// ============================================================================
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"

namespace zenith::obs {

struct ZENITH_API TraceContext {
    std::string traceId; // 32 位小写十六进制
    std::string spanId;  // 16 位小写十六进制
    bool sampled = true;

    [[nodiscard]] bool valid() const noexcept;

    /// 序列化为 `00-<trace>-<span>-<flags>`（不符合规范时返回空串）
    [[nodiscard]] std::string traceparent() const;

    /// 派生下一跳：沿用 traceId，换新 spanId
    [[nodiscard]] TraceContext child() const;

    /// 生成全新根上下文
    static TraceContext generate();

    /// 解析 traceparent；格式非法返回 nullopt（校验：长度/小写十六进制/
    /// 全零 id/版本 ff 拒绝）
    static std::optional<TraceContext> parse(std::string_view header);

    /// 便捷：解析失败时生成新的根上下文（服务端入口的常见策略）
    static TraceContext parseOrGenerate(std::string_view header);
};

/// 线程内当前链路（nullopt = 无）
ZENITH_API std::optional<TraceContext> currentTrace();
ZENITH_API void setCurrentTrace(std::optional<TraceContext> tc);

/// 作用域守卫：进入时设置，离开时恢复原值（服务端每请求一个）
class ZENITH_API TraceScope {
public:
    explicit TraceScope(std::optional<TraceContext> tc);
    ~TraceScope();
    TraceScope(const TraceScope&) = delete;
    TraceScope& operator=(const TraceScope&) = delete;
    TraceScope(TraceScope&&) = delete;
    TraceScope& operator=(TraceScope&&) = delete;

private:
    std::optional<TraceContext> saved_;
};

} // namespace zenith::obs
