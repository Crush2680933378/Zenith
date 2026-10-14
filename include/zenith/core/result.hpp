// ============================================================================
//  Result<T> —— 无异常路径的返回值封装（C++17，仅标准库）
//  用法：
//    Result<int> r = compute();
//    if (r) { use(r.value()); } else { log(r.error().message); }
// ============================================================================
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace zenith::core {

/// 通用错误类型：code 由各模块自定义，message 供日志/展示。
struct Error {
    int         code{0};
    std::string message;

    Error() = default;
    Error(int c, std::string msg) : code(c), message(std::move(msg)) {}
};

template <typename T>
class [[nodiscard]] Result {
public:
    static Result Ok(T value) { return Result(std::move(value)); }
    static Result Err(Error e) { return Result(std::move(e)); }

    [[nodiscard]] bool isOk() const noexcept { return value_.has_value(); }
    [[nodiscard]] bool isErr() const noexcept { return !value_.has_value(); }
    [[nodiscard]] explicit operator bool() const noexcept { return isOk(); }

    T&       value() &          { return value_.value(); }
    const T& value() const&     { return value_.value(); }
    T&&      value() &&         { return std::move(value_).value(); }

    [[nodiscard]] const Error& error() const { return error_.value(); }

    T valueOr(T fallback) const& { return value_.value_or(std::move(fallback)); }
    T valueOr(T fallback) &&     { return std::move(value_).value_or(std::move(fallback)); }

private:
    Result(T value) : value_(std::move(value)) {}
    Result(Error e) : error_(std::move(e)) {}

    std::optional<T>     value_;
    std::optional<Error> error_;
};

/// Result<void> 特化：只表达成功/失败。
template <>
class [[nodiscard]] Result<void> {
public:
    static Result Ok() { return Result(true); }
    static Result Err(Error e) { return Result(std::move(e)); }

    [[nodiscard]] bool isOk() const noexcept { return ok_; }
    [[nodiscard]] bool isErr() const noexcept { return !ok_; }
    [[nodiscard]] explicit operator bool() const noexcept { return ok_; }

    [[nodiscard]] const Error& error() const { return error_.value(); }

private:
    explicit Result(bool ok) : ok_(ok) {}
    explicit Result(Error e) : ok_(false), error_(std::move(e)) {}

    bool                 ok_;
    std::optional<Error> error_;
};

} // namespace zenith::core
