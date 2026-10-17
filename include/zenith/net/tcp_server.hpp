// ============================================================================
//  zenith::net::TcpServer —— TCP 服务器（accept 在主 loop，IO 分发到线程组）
// ============================================================================
#pragma once

#include <functional>
#include <unordered_map>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"
#include "zenith/net/socket.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::net {

class ZENITH_API TcpServer : private core::NonCopyable {
public:
    TcpServer(EventLoop* loop, EventLoopThreadPool* ioPool = nullptr);
    ~TcpServer();

    /// port 传 0 使用临时端口（listen 后 port() 查询实际值）。
    core::Result<void> listen(const std::string& host, uint16_t port, int backlog = 128);

    void setConnectionCallback(TcpConnection::ConnectCallback cb) { onConn_ = std::move(cb); }
    void setMessageCallback(TcpConnection::MessageCallback cb)    { onMsg_ = std::move(cb); }

    /// 连接装饰器：accept 后、establish 前调用（用于安装 TLS 滤层等）。
    /// 可叠加多次（如 HttpServer 防护计数 + TLS 滤层）——按注册序执行。
    void addConnectionDecorator(std::function<void(const TcpConnection::Ptr&)> d) {
        decorators_.push_back(std::move(d));
    }
    /// 兼容旧名（覆盖语义）
    void setConnectionDecorator(std::function<void(const TcpConnection::Ptr&)> d) {
        decorators_.clear();
        decorators_.push_back(std::move(d));
    }

    void stop();

    [[nodiscard]] uint16_t port() const noexcept { return port_; }

private:
    void handleAccept();
    void removeConnection(const TcpConnection::Ptr& conn);

    EventLoop* loop_;
    EventLoopThreadPool* ioPool_; // 可空：单 loop 模式
    Socket listenSock_;
    uint16_t port_ = 0;

    std::unordered_map<fd_t, TcpConnection::Ptr> conns_; // 仅主 loop 线程
    TcpConnection::ConnectCallback onConn_;
    TcpConnection::MessageCallback onMsg_;
    std::vector<std::function<void(const TcpConnection::Ptr&)>> decorators_;
};

} // namespace zenith::net
