// ============================================================================
//  zenith::net::TcpClient —— 一次性非阻塞连接客户端（含超时）
//  - 必须以 std::make_shared<TcpClient>(loop) 创建（内部 shared_from_this）
//  - 连接成功后 fd 所有权移交 TcpConnection；本对象随后自动消亡
//  - M2 的 DNS 为阻塞 getaddrinfo（调用线程）；异步 DNS 后续里程碑
// ============================================================================
#pragma once

#include <functional>
#include <memory>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/socket.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::net {

struct ConnectOptions {
    std::string host{"127.0.0.1"};
    uint16_t port = 0;
    int timeoutMs = 5000;
};

class ZENITH_API TcpClient : private core::NonCopyable,
                             public std::enable_shared_from_this<TcpClient> {
public:
    using Ptr = std::shared_ptr<TcpClient>;
    using ConnectedCallback = std::function<void(const TcpConnection::Ptr&)>;
    using FailedCallback    = std::function<void(const std::string& reason)>;

    explicit TcpClient(EventLoop* loop);
    ~TcpClient();

    /// 发起连接（每实例一次；须在 shared_ptr 上调用）。
    void connect(const ConnectOptions& opt, ConnectedCallback onOk, FailedCallback onFail);

    /// 取消未完成的连接。
    void cancel();

private:
    void succeed();
    void fail(const std::string& reason);
    void cleanupRegistration();

    EventLoop* loop_;
    Socket sock_;
    uint64_t timerId_ = 0;
    bool pending_ = false;
    ConnectedCallback onOk_;
    FailedCallback onFail_;
};

} // namespace zenith::net
