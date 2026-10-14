#include "zenith/net/tcp_server.hpp"

namespace zenith::net {

TcpServer::TcpServer(EventLoop* loop, EventLoopThreadPool* ioPool)
    : loop_(loop), ioPool_(ioPool) {}

TcpServer::~TcpServer() { stop(); }

core::Result<void> TcpServer::listen(const std::string& host, uint16_t port, int backlog) {
    auto sockR = Socket::createTcp();
    if (!sockR) return core::Result<void>::Err(std::move(sockR).error());
    Socket sock = std::move(sockR).value();
    sock.setReuseAddr(true);

    auto br = sock.bind(host, port);
    if (!br) return core::Result<void>::Err(std::move(br).error());
    auto pr = sock.localPort();
    if (pr) port_ = pr.value();

    auto lr = sock.listen(backlog);
    if (!lr) return core::Result<void>::Err(std::move(lr).error());
    if (port_ == 0) {
        auto p2 = sock.localPort();
        if (p2) port_ = p2.value();
    }

    listenSock_ = std::move(sock);

// 监听注册投递到 loop 线程（registerFd 仅 loop 线程调用；
    // loop 尚未启动时 runInLoop 内联执行，行为不变）
    TcpServer* self = this;
    const fd_t listenFd = listenSock_.fd();
    loop_->runInLoop([self, listenFd] {
        self->loop_->registerFd(listenFd, kPollReadable,
                                [self](uint16_t) { self->handleAccept(); });
    });
    return core::Result<void>::Ok();
}

void TcpServer::handleAccept() {
    for (;;) {
        Endpoint peer;
        auto cr = listenSock_.accept(&peer);
        if (!cr) {
            // accept 真错误：记录后继续（监听 fd 仍有效）
            continue;
        }
        const fd_t cfd = cr.value();
        if (cfd == kInvalidFd) return; // EAGAIN

        EventLoop* ioLoop = ioPool_ != nullptr ? ioPool_->nextLoop() : loop_;
        auto conn = TcpConnection::create(ioLoop, cfd, peer.host + ":" + std::to_string(peer.port));
        conn->setConnectCallback(onConn_);
        conn->setMessageCallback(onMsg_);
        conn->finalize([this](const TcpConnection::Ptr& c) { removeConnection(c); });
        if (decorator_) decorator_(conn); // TLS 滤层等安装（establish 前）

        // 连接表在主 loop 维护（accept 顺序确定）；
        // fd 注册与回调必须发生在所属 IO loop 线程内（跨线程经 runInLoop 投递）
        conns_[cfd] = conn;
        ioLoop->runInLoop([conn] { conn->establish(); });
    }
}

void TcpServer::removeConnection(const TcpConnection::Ptr& conn) {
    loop_->runInLoop([this, conn] { conns_.erase(conn->fd()); });
}

void TcpServer::stop() {
    if (!listenSock_.valid()) return;
    loop_->deregisterFd(listenSock_.fd());
    listenSock_.close();
    // 连接交由各自 IO loop 关闭：向每条连接投递 forceClose
    for (auto& [fd, conn] : conns_) {
        if (conn) conn->forceClose();
    }
    conns_.clear();
}

} // namespace zenith::net
