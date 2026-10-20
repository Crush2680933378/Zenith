#include "zenith/log/console_logger.hpp"

#include <cstdio>

#if defined(ZENITH_PLATFORM_WINDOWS)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <unistd.h>
#endif

namespace zenith::log {

namespace {

constexpr const char* kAnsiDim    = "\x1b[2m";
constexpr const char* kAnsiCyan   = "\x1b[36m";
constexpr const char* kAnsiGreen  = "\x1b[32m";
constexpr const char* kAnsiYellow = "\x1b[33m";
constexpr const char* kAnsiRed    = "\x1b[31m";
constexpr const char* kAnsiBoldRed = "\x1b[1;31m";
constexpr const char* kAnsiReset  = "\x1b[0m";

const char* levelAnsiColor(LogLevel level) {
    switch (level) {
        case LogLevel::Trace: return kAnsiDim;
        case LogLevel::Debug: return kAnsiCyan;
        case LogLevel::Info:  return kAnsiGreen;
        case LogLevel::Warn:  return kAnsiYellow;
        case LogLevel::Error: return kAnsiRed;
        case LogLevel::Fatal: return kAnsiBoldRed;
    }
    return kAnsiReset;
}

#if defined(ZENITH_PLATFORM_WINDOWS)
/// Windows 10+ 控制台启用 VT 处理；失败则维持纯文本。进程内只做一次。
bool enableVirtualTerminalOnce() {
    static const bool ok = [] {
        HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(h, &mode)) return false;
        return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
    }();
    return ok;
}

/// Windows 控制台默认代码页是 GBK（936），而 Zenith 的日志/HTTP 消息全部是 UTF-8
/// 字面量，直接打印中文必乱码。这里把输入/输出代码页都切到 UTF-8。
/// GetConsoleCP 在输出被重定向到文件/管道时失败（返回 0）——那正是我们想要的：
/// 重定向场景不需要（也不应该）改代码页，字节原样落盘即正确。
/// 幂等：进程内只做一次；教学要点——UTF-8 日志系统在 Windows 上的第一件事。
bool enableUtf8CodePageOnce() {
    static const bool ok = [] {
        const UINT out = GetConsoleOutputCP();
        if (out == 0 || out == 65001) return out == 65001;
        const BOOL o = SetConsoleOutputCP(65001);
        const BOOL i = SetConsoleCP(65001);
        return o != FALSE && i != FALSE;
    }();
    return ok;
}
#endif

bool colorSupported() {
#if defined(ZENITH_PLATFORM_WINDOWS)
    return enableVirtualTerminalOnce();
#else
    return isatty(STDERR_FILENO) != 0 || isatty(STDOUT_FILENO) != 0;
#endif
}

} // namespace

ConsoleLogger::ConsoleLogger(bool useStderr, bool colorEnabled)
    : useStderr_(useStderr), color_(colorEnabled && colorSupported()) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    // 无论是否开彩色，控制台代码页都要切到 UTF-8——否则纯文本中文一样乱码。
    (void)enableUtf8CodePageOnce();
#endif
}

void ConsoleLogger::write(const LogRecord& record) {
    const std::string plain = formatLogLine(record); // 以 '\n' 结尾
    FILE* out = useStderr_ ? stderr : stdout;
    if (color_) {
        std::fputs(levelAnsiColor(record.level), out);
        std::fwrite(plain.data(), 1, plain.size() - 1, out); // 去掉尾部 '\n'
        std::fputs(kAnsiReset, out);
        std::fputc('\n', out);
    } else {
        std::fwrite(plain.data(), 1, plain.size(), out);
    }
}

void ConsoleLogger::flush() {
    std::fflush(useStderr_ ? stderr : stdout);
}

} // namespace zenith::log
