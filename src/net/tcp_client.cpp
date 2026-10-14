#include "zenith/net/tcp_client.hpp"

#include "net_detail.hpp"

namespace zenith::net {

TcpClient::TcpClient(EventLoop* loop) : loop_(loop) {}

TcpClient::~TcpClient() { cancel(); }

void TcpClient::connect(const ConnectOptions& opt, ConnectedCallback onOk, FailedCallback onFail) {
    if (pending_) return;
    onOk_   = std::move(onOk);
    onFail_ = std::move(onFail);

    auto sock = Socket::createTcp();
    if (!sock) {
        fail(std::move(sock).error().message);
        return;
    }
    sock_ = std::move(sock).value();
    sock_.setNoDelay(true);

    auto cr = sock_.connect(opt.host, opt.port);
    if (!cr) {
        fail(std::move(cr).error().message);
        return;
    }

    pending_ = true;

    // 所有延后回调持有 self，保证对象存活至连接判定完成
    Ptr self = shared_from_this();

    timerId_ = loop_->scheduleAfter(std::chrono::milliseconds(opt.timeoutMs), [self] {
        if (self->pending_) self->fail("connect timeout");
    });

    if (cr.value()) {
        loop_->runInLoop([self] { self->succeed(); }); // 立即连上（如 loopback）
        return;
    }

    loop_->registerFd(sock_.fd(), kPollWritable, [self](uint16_t revents) {
        if (!self->pending_) return;
        if ((revents & (kPollError | kPollHangup)) != 0) {
            self->fail("connect failed: " + Socket::lastSystemError());
            return;
        }
        const int soErr = detail::getSocketError(self->sock_.fd());
        if (soErr != 0) {
            self->fail("connect failed (SO_ERROR=" + std::to_string(soErr) + ")");
            return;
        }
        self->succeed();
    });
}

void TcpClient::cleanupRegistration() {
    if (timerId_ != 0) {
        loop_->cancel(timerId_);
        timerId_ = 0;
    }
    if (sock_.valid()) {
        loop_->deregisterFd(sock_.fd());
    }
}

void TcpClient::succeed() {
    if (!pending_) return;
    pending_ = false;
    cleanupRegistration();

    const Endpoint peer = sock_.peerEndpoint();
    auto conn = TcpConnection::create(loop_, sock_.releaseFd(),
                                      peer.host + ":" + std::to_string(peer.port));
    conn->establish(); // fd 所有权移交连接对象

    ConnectedCallback cb = std::move(onOk_);
    onFail_ = nullptr;
    if (cb) cb(conn);
}

void TcpClient::fail(const std::string& reason) {
    const bool wasPending = pending_;
    pending_ = false;
    cleanupRegistration();
    sock_.close();

    FailedCallback cb = std::move(onFail_);
    onOk_ = nullptr;
    if (wasPending || cb) {
        if (cb) cb(reason);
    }
}

void TcpClient::cancel() {
    if (!pending_) return;
    pending_ = false;
    cleanupRegistration();
    sock_.close();
    onOk_ = nullptr;
    onFail_ = nullptr;
}

} // namespace zenith::net
