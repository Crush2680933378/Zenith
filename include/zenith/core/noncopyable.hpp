// ============================================================================
//  禁止拷贝基类
// ============================================================================
#pragma once

namespace zenith::core {

class NonCopyable {
protected:
    NonCopyable() = default;
    ~NonCopyable() = default;

    NonCopyable(const NonCopyable&) = delete;
    NonCopyable& operator=(const NonCopyable&) = delete;
};

} // namespace zenith::core
