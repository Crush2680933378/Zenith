#include "zenith/log/logger.hpp"

#include <functional>
#include <thread>

namespace zenith::log {

const char* toString(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO ";
        case LogLevel::Warn:  return "WARN ";
        case LogLevel::Error: return "ERROR";
        case LogLevel::Fatal: return "FATAL";
    }
    return "?????";
}

std::string formatLogLine(const LogRecord& r) {
    std::string line;
    line.reserve(r.message.size() + 64);
    line += '[';
    line += core::formatTimestamp(r.timestampMs);
    line += "] [";
    line += toString(r.level);
    line += "] [tid ";
    line += std::to_string(r.threadId);
    line += "] ";
    // 只取文件名，避免超长路径
    const char* base = r.file;
    for (const char* p = r.file; *p; ++p) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }
    line += base;
    line += ':';
    line += std::to_string(r.line);
    line += "] ";
    line += r.message;
    line += '\n';
    return line;
}

LoggerFacade& LoggerFacade::instance() {
    static LoggerFacade facade;
    return facade;
}

void LoggerFacade::addSink(std::shared_ptr<LogSink> sink) {
    if (!sink) return;
    std::lock_guard<std::mutex> lk(mtx_);
    sinks_.push_back(std::move(sink));
}

void LoggerFacade::clearSinks() {
    std::lock_guard<std::mutex> lk(mtx_);
    sinks_.clear();
}

void LoggerFacade::shutdown() {
    clearSinks();
}

void LoggerFacade::setLevel(LogLevel level) noexcept { level_ = level; }

LogLevel LoggerFacade::level() const noexcept { return level_; }

bool LoggerFacade::enabled(LogLevel level) const noexcept {
    return static_cast<int>(level) >= static_cast<int>(level_);
}

void LoggerFacade::write(LogLevel level, const char* file, int line, std::string message) {
    if (!enabled(level)) return; // 与 writeT 宏路径保持同一语义

    LogRecord r;
    r.level       = level;
    r.timestampMs = core::nowMilliseconds();
    r.threadId    = std::hash<std::thread::id>{}(std::this_thread::get_id());
    r.file        = file;
    r.line        = line;
    r.message     = std::move(message);

    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& sink : sinks_) sink->write(r);
}

void LoggerFacade::writeRaw(LogLevel level, const char* file, int line, const char* message) {
    write(level, file, line, std::string(message ? message : "(null)"));
}

void LoggerFacade::flush() {
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto& sink : sinks_) sink->flush();
}

} // namespace zenith::log
