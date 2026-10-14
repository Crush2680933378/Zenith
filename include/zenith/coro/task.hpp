// ============================================================================
//  zenith::coro::Task<T> —— 可选协程层（ZENITH_ENABLE_COROUTINES=ON，C++20）
//
//  - 惰性启动：创建协程不执行，co_await / syncWait 时才运行
//  - 对称转移：协程结束直接切换回等待者，无多余栈跳转
//  - 异常传播：协程体内异常在 co_await 处重新抛出
//  - M2 接入 EventLoop 后，I/O awaiter 在事件回调中 resume() 协程；
//    届时 co_await tcp_connect(...) / http_fetch(...) 即为正式异步形态。
//
//  本层与核心库（C++17）完全解耦：关闭选项时核心不依赖任何 C++20 设施。
// ============================================================================
#pragma once

#include <cassert>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace zenith::coro {

template <typename T>
class Task;

namespace detail {

struct PromiseBase {
    struct FinalAwaiter {
        [[nodiscard]] bool await_ready() const noexcept { return false; }

        template <typename P>
        std::coroutine_handle<> await_suspend(std::coroutine_handle<P> h) noexcept {
            // 对称转移：直接切回等待者；无人等待则回到 resumer（顶层）
            auto& p = h.promise();
            return p.continuation ? p.continuation : std::noop_coroutine();
        }

        void await_resume() const noexcept {}
    };

    std::coroutine_handle<> continuation = nullptr; // co_await 者

    [[nodiscard]] std::suspend_always initial_suspend() const noexcept { return {}; }
    [[nodiscard]] FinalAwaiter final_suspend() const noexcept { return {}; }
};

template <typename T>
struct TaskPromise final : PromiseBase {
    std::optional<T> value;
    std::exception_ptr exception;

    Task<T> get_return_object();

    void return_value(T v) { value.emplace(std::move(v)); }
    void unhandled_exception() { exception = std::current_exception(); }

    [[nodiscard]] T&& result() {
        if (exception) std::rethrow_exception(exception);
        assert(value.has_value());
        return std::move(*value);
    }
};

template <>
struct TaskPromise<void> final : PromiseBase {
    std::exception_ptr exception;

    Task<void> get_return_object();
    void return_void() noexcept {}
    void unhandled_exception() { exception = std::current_exception(); }
    void result() {
        if (exception) std::rethrow_exception(exception);
    }
};

} // namespace detail

template <typename T = void>
class [[nodiscard]] Task {
public:
    using promise_type = detail::TaskPromise<T>;
    using Handle       = std::coroutine_handle<promise_type>;

    Task() noexcept = default;
    explicit Task(Handle h) noexcept : handle_(h) {}
    Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    Task& operator=(Task&& other) noexcept {
        if (this != &other) {
            destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    ~Task() { destroy(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }

    /// Task 自身即 awaiter：co_await task 时挂起当前协程、启动被等待者。
    [[nodiscard]] bool await_ready() const noexcept { return handle_ == nullptr; }

    [[nodiscard]] std::coroutine_handle<>
    await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation = awaiter;
        return handle_; // 对称转移：直接开始执行被等待协程
    }

    [[nodiscard]] decltype(auto) await_resume() { return handle_.promise().result(); }

    Handle handle() const noexcept { return handle_; }

    void destroy() {
        if (handle_) {
            handle_.destroy();
            handle_ = nullptr;
        }
    }

private:
    Handle handle_ = nullptr;
};

namespace detail {
template <typename T>
Task<T> TaskPromise<T>::get_return_object() {
    return Task<T>{std::coroutine_handle<TaskPromise<T>>::from_promise(*this)};
}
inline Task<void> TaskPromise<void>::get_return_object() {
    return Task<void>{std::coroutine_handle<TaskPromise<void>>::from_promise(*this)};
}
} // namespace detail

// ---------------------------------------------------------------------------
// syncWait —— 无事件循环时的顶层驱动：同步运行惰性协程直至完成。
// M2 EventLoop 就位后新增 loop.run(task) 调度式入口。
// ---------------------------------------------------------------------------
template <typename T>
T syncWait(Task<T> task) {
    if (!task.valid()) throw std::invalid_argument("syncWait: empty task");
    task.handle().resume(); // 惰性启动；内部若无 I/O 挂起将直接跑完
    return task.await_resume();
}

} // namespace zenith::coro
