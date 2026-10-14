// ============================================================================
//  ConsoleLogger —— 控制台彩色日志 sink
//  - Windows 10+ 自动启用 VT100 转义；不支持时优雅退化为纯文本
//  - 默认输出到 stderr（保证与 stdout 业务输出不互相穿插）
// ============================================================================
#pragma once

#include "zenith/core/export.hpp"
#include "zenith/log/logger.hpp"

namespace zenith::log {

class ZENITH_API ConsoleLogger : public LogSink {
public:
    /// colorEnabled=true 时尽量启用 ANSI 颜色（自动探测终端支持）。
    explicit ConsoleLogger(bool useStderr = true, bool colorEnabled = true);

    void write(const LogRecord& record) override;
    void flush() override;

private:
    bool useStderr_;
    bool color_;
};

} // namespace zenith::log
