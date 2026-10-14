// ============================================================================
//  Zenith 日志门面
//  - LogSink：输出端抽象（控制台 / 异步文件 / 未来的网络上报…）
//  - LoggerFacade：进程级单例，持有若干 sink，ZLOG_* 宏是唯一推荐入口
//  - 线程安全：addSink 与 write 可并发；写路径为格式化 + 逐 sink 分发
// ============================================================================
#pragma once

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/time_util.hpp"

namespace zenith::log {

enum class LogLevel : int {
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
    Fatal = 5,
};

ZENITH_API const char* toString(LogLevel level) noexcept;

struct LogRecord {
    LogLevel    level{LogLevel::Info};
    int64_t     timestampMs{0};   // Unix 毫秒
    uint64_t    threadId{0};      // hash(thread::id)
    const char* file{""};         // 编译期字符串
    int         line{0};
    std::string message;
};

class LogSink {
public:
    virtual ~LogSink() = default;
    virtual void write(const LogRecord& record) = 0;
    virtual void flush() {}
};

/// 单条日志的统一文本格式（不带颜色）。AsyncFileLogger 等复用。
ZENITH_API std::string formatLogLine(const LogRecord& record);

class ZENITH_API LoggerFacade {
public:
    static LoggerFacade& instance();

    void addSink(std::shared_ptr<LogSink> sink);
    void clearSinks();

    /// 宿主退出前显式调用：销毁全部 sink（含异步 sink 的后台线程 join）。
    /// 必须在 main 返回前调用——若拖到 DLL_PROCESS_DETACH 阶段析构，
    /// join 会与加载器锁互锁导致进程无法退出（Windows 动态库形态）。
    void shutdown();
    void setLevel(LogLevel level) noexcept;
    [[nodiscard]] LogLevel level() const noexcept;
    [[nodiscard]] bool enabled(LogLevel level) const noexcept;

    /// ZLOG 宏入口：格式化后分发到全部 sink。
    void write(LogLevel level, const char* file, int line, std::string message);

    /// 变参模板入口：真正的参数包折叠在这里完成（宏里的 __VA_ARGS__
    /// 无法直接参与折叠表达式——顶层逗号会被拆开）。
    template <typename... Args>
    void writeT(LogLevel level, const char* file, int line, Args&&... args) {
        if (!enabled(level)) return;
        std::ostringstream zos;
        (zos << ... << std::forward<Args>(args));
        write(level, file, line, zos.str());
    }

    /// C ABI thunk 入口：消息已是最终文本（用于插件侧日志）。
    void writeRaw(LogLevel level, const char* file, int line, const char* message);

    void flush();

private:
    LoggerFacade() = default;

    mutable std::mutex                 mtx_;
    std::vector<std::shared_ptr<LogSink>> sinks_;
    LogLevel                           level_ = LogLevel::Info;
};

} // namespace zenith::log

// ---------------------------------------------------------------------------
// 宏入口：流式拼接，逗号分隔传参，如 ZLOG_INFO("count = ", 42)
// ---------------------------------------------------------------------------
#define ZLOG_WRITE(zlevel, ...)                                                        \
    do {                                                                               \
        ::zenith::log::LoggerFacade::instance().writeT(                                \
            zlevel, __FILE__, __LINE__, __VA_ARGS__);                                  \
    } while (0)

#define ZLOG_TRACE(...) ZLOG_WRITE(::zenith::log::LogLevel::Trace, __VA_ARGS__)
#define ZLOG_DEBUG(...) ZLOG_WRITE(::zenith::log::LogLevel::Debug, __VA_ARGS__)
#define ZLOG_INFO(...)  ZLOG_WRITE(::zenith::log::LogLevel::Info,  __VA_ARGS__)
#define ZLOG_WARN(...)  ZLOG_WRITE(::zenith::log::LogLevel::Warn,  __VA_ARGS__)
#define ZLOG_ERROR(...) ZLOG_WRITE(::zenith::log::LogLevel::Error, __VA_ARGS__)
#define ZLOG_FATAL(...) ZLOG_WRITE(::zenith::log::LogLevel::Fatal, __VA_ARGS__)
