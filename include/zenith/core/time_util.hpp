// ============================================================================
//  时间工具（纯头文件实现）
// ============================================================================
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

#include "zenith/core/platform.hpp"

namespace zenith::core {

/// 挂钟时间（毫秒，自 Unix 纪元）——用于日志时间戳。
inline int64_t nowMilliseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 挂钟时间（微秒，自 Unix 纪元）。
inline int64_t nowMicroseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 单调时钟（微秒）——用于耗时测量，不受系统改时间影响。
inline int64_t steadyMicroseconds() noexcept {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// "YYYY-MM-DD HH:MM:SS.mmm"（本地时区）。
inline std::string formatTimestamp(int64_t msSinceEpoch) {
    const time_t seconds = static_cast<time_t>(msSinceEpoch / 1000);
    const int    millis  = static_cast<int>(msSinceEpoch % 1000);

    std::tm tmv{};
#if defined(ZENITH_PLATFORM_WINDOWS)
    localtime_s(&tmv, &seconds);
#else
    localtime_r(&seconds, &tmv);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, millis);
    return buf;
}

} // namespace zenith::core
