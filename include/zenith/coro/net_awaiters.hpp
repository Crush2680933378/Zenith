// ============================================================================
//  zenith::coro 网络等待器 —— 把回调式异步 API 变成 co_await（C++20 可选层）
//  用法（EventLoop 需在其它线程运行，或同线程场景下配合 loop 派发）：
//    EventLoop loop; std::thread t([&]{ loop.loop(); });
//    auto resp = zenith::coro::syncWait(zenith::coro::httpFetch(loop, req));
//    loop.stop(); t.join();
//  完成回调在 loop 线程 resume 协程 → 对称转移汇合回 syncWait。
// ============================================================================
#pragma once

#include <coroutine>
#include <optional>
#include <stdexcept>
#include <utility>

#include "zenith/coro/task.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/net/tcp_client.hpp"

namespace zenith::coro {

namespace detail {

/// 把「带回调的异步操作」挂起为 co_await。
/// Starter 形如 void(std::function<void(core::Result<T>)>)。
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

} // namespace detail

/// co_await tcpConnect(loop, host, port) → Task<TcpConnection::Ptr>；失败抛异常。
inline Task<net::TcpConnection::Ptr>
tcpConnect(net::EventLoop& loop, std::string host, uint16_t port, int timeoutMs = 5000) {
    auto client = std::make_shared<net::TcpClient>(&loop);
    co_return co_await detail::awaitCallback<net::TcpConnection::Ptr>(
        [client, host = std::move(host), port, timeoutMs](auto cb) {
            net::ConnectOptions opt;
            opt.host = host;
            opt.port = port;
            opt.timeoutMs = timeoutMs;
            client->connect(opt,
                            [cb](const net::TcpConnection::Ptr& conn) {
                                cb(core::Result<net::TcpConnection::Ptr>::Ok(conn));
                            },
                            [cb](const std::string& reason) {
                                cb(core::Result<net::TcpConnection::Ptr>::Err(
                                    core::Error(50, "tcp connect: " + reason)));
                            });
        });
}

/// co_await httpFetch(loop, req) → Task<HttpResponse>；失败抛异常。
inline Task<http::HttpResponse> httpFetch(net::EventLoop& loop,
                                          const http::ClientRequest& req) {
    // HttpClient 实例由 Starter 捕获保活直至回调触发
    auto client = std::make_shared<http::HttpClient>(&loop);
    co_return co_await detail::awaitCallback<http::HttpResponse>(
        [client, &req](auto cb) { client->request(req, cb); });
}

} // namespace zenith::coro
