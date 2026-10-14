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
    : useStderr_(useStderr), color_(colorEnabled && colorSupported()) {}

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
