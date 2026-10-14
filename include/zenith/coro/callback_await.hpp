// ============================================================================
//  zenith::coro::detail::awaitCallback —— 通用「回调转协程」挂起器
//  Starter 形如 void(std::function<void(core::Result<T>)>)。
//  完成回调在任意线程 resume 协程；对称转移汇合回 syncWait。
// ============================================================================
#pragma once

#include <coroutine>
#include <optional>
#include <stdexcept>
#include <utility>

#include "zenith/coro/task.hpp"
#include "zenith/core/result.hpp"

namespace zenith::coro::detail {

template <typename T, typename Starter>
Task<T> awaitCallback(Starter starter) {
    struct Awaiter {
        Starter starter;
        std::optional<core::Result<T>> result;

        [[nodiscard]] bool await_ready() const noexcept { return false; }

        void await_suspend(std::coroutine_handle<> h) {
            starter([this, h](core::Result<T> r) {
                result.emplace(std::move(r));
                h.resume(); // 通常在 loop 线程；对称转移回协程
            });
        }

        T await_resume() {
            if (!result.has_value() || result->isErr()) {
                throw std::runtime_error(!result.has_value()
                                             ? std::string("coro awaiter: no result")
                                             : result->error().message);
            }
            return std::move(result->value());
        }
    };

    co_return co_await Awaiter{std::move(starter), std::nullopt};
}

} // namespace zenith::coro::detail
