// ============================================================================
//  zenith::http::HttpServer —— 基于 TcpServer 的 HTTP/1.1 服务器
//  - keep-alive / HEAD / 异常捕获(500) / BadRequestBody(400)
//  - 每连接附带 HttpRequestParser（connection context）
// ============================================================================
#pragma once

#include <memory>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_parser.hpp"
#include "zenith/http/router.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"
#include "zenith/net/tcp_server.hpp"
#include "zenith/net/websocket.hpp"

namespace zenith::http {

class ZENITH_API HttpServer : private core::NonCopyable {
public:
    HttpServer(net::EventLoop* loop, net::EventLoopThreadPool* ioPool = nullptr,
               size_t maxBodyBytes = 10 * 1024 * 1024);
    ~HttpServer();

    core::Result<void> listen(const std::string& host = "0.0.0.0", uint16_t port = 8080,
                              int backlog = 128);

    /// 访问内部路由表（注册路由/中间件/分组）。
    [[nodiscard]] Router& router() noexcept { return router_; }

    /// 注册 WebSocket 升级路径（精确匹配 path）。
    /// onOpen：升级完成后触发（在连接所属 IO 线程）；会话回调由用户继续设置。
    void enableWebSocket(const std::string& path,
                         std::function<void(const net::WebSocketSession::Ptr&)> onOpen);

    void stop();

    [[nodiscard]] uint16_t port() const noexcept { return server_.port(); }

private:
    void onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf);
    void handleRequest(const net::TcpConnection::Ptr& conn);
    /// 返回 true 表示已完成 WebSocket 升级（连接被会话接管）
    bool tryWebSocketUpgrade(const net::TcpConnection::Ptr& conn);

    net::EventLoop* loop_;
    net::TcpServer server_;
    Router router_;
    size_t maxBodyBytes_;
    std::unordered_map<std::string, std::function<void(const net::WebSocketSession::Ptr&)>>
        wsPaths_;
};

} // namespace zenith::http
