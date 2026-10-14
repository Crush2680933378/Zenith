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

#include "zenith/coro/callback_await.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/net/tcp_client.hpp"

namespace zenith::coro {

using detail::awaitCallback; // 共享实现见 callback_await.hpp

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
