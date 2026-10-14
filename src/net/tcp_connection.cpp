#include "zenith/net/tcp_connection.hpp"

#include "net_detail.hpp"

namespace zenith::net {

TcpConnection::TcpConnection(EventLoop* loop, fd_t fd, std::string peer)
    : loop_(loop), fd_(fd), peer_(std::move(peer)), sock_(fd) {}

TcpConnection::~TcpConnection() {
    // fd_ 由 Socket 析构统一关闭；deregister 由 handleClose 完成
}

void TcpConnection::establish() {
    connected_.store(true, std::memory_order_release);
    auto self = shared_from_this();
    loop_->registerFd(fd_, kPollReadable, [self](uint16_t revents) {
        self->handleEvents(revents);
    });
    if (onConnect_) onConnect_(self);
}

void TcpConnection::finalize(CloseCallback hostClose) { hostClose_ = std::move(hostClose); }

void TcpConnection::send(const void* data, size_t n) {
    if (!connected_.load(std::memory_order_acquire) || n == 0) return;
    auto self = shared_from_this();
    loop_->runInLoop([self, d = std::string(static_cast<const char*>(data), n)] {
        self->sendInLoop(d.data(), d.size());
    });
}

void TcpConnection::sendInLoop(const void* data, size_t n) {
    if (!connected_.load(std::memory_order_acquire)) return;
    if (writing_ || outBuf_.readableBytes() > 0) {
        outBuf_.append(data, n);
        return;
    }
    const int w = sock_.writeSome(static_cast<const char*>(data), n);
    size_t remaining = n;
    if (w > 0) remaining = n - static_cast<size_t>(w);
    if (remaining > 0) {
        outBuf_.append(static_cast<const char*>(data) + (n - remaining), remaining);
        enableWriteInterest(true);
    } else if (w == -2) {
        handleClose("send error: " + Socket::lastSystemError());
        return;
    }
    if (shutdownPending_ && outBuf_.readableBytes() == 0) {
        // 已排空且收到 shutdown 请求 → 真正半关闭
        detail::shutdownWrite(fd_);
    }
}

void TcpConnection::shutdown() {
    auto self = shared_from_this();
    loop_->runInLoop([self] {
        if (!self->connected_.load()) return;
        if (self->outBuf_.readableBytes() == 0) {
            detail::shutdownWrite(self->fd_);
        } else {
            self->shutdownPending_ = true; // 排空后再关
        }
    });
}

void TcpConnection::forceClose() {
    auto self = shared_from_this();
    loop_->runInLoop([self] { self->handleClose("force close"); });
}

void TcpConnection::enableWriteInterest(bool on) {
    writing_ = on;
    loop_->updateFd(fd_, kPollReadable | (on ? kPollWritable : 0));
}

void TcpConnection::handleEvents(uint16_t revents) {
    if ((revents & (kPollError | kPollHangup)) != 0) {
        // 优先读尽数据再判断关闭（POLLHUP 时可能仍有数据可读）
        handleRead();
        if (connected_.load()) handleClose("hangup/error event");
        return;
    }
    if ((revents & kPollWritable) != 0) handleWrite();
    if ((revents & kPollReadable) != 0) handleRead();
}

void TcpConnection::handleRead() {
    char buf[65536];
    for (;;) {
        const int n = sock_.readSome(buf, sizeof(buf));
        if (n > 0) {
            inBuf_.append(buf, static_cast<size_t>(n));
            if (n < static_cast<int>(sizeof(buf))) break; // 读尽本轮
            continue;
        }
        if (n == -1) break;                       // EAGAIN
        if (n == 0) {                             // 对端关闭
            handleClose("peer closed");
            return;
        }
        handleClose("recv error: " + Socket::lastSystemError());
        return;
    }
    if (connected_.load() && onMessage_ && inBuf_.readableBytes() > 0) {
        onMessage_(shared_from_this(), inBuf_);
    }
}

void TcpConnection::handleWrite() {
    if (!connected_.load()) return;
    while (outBuf_.readableBytes() > 0) {
        const int w = sock_.writeSome(reinterpret_cast<const char*>(outBuf_.peek()),
                                      outBuf_.readableBytes());
        if (w > 0) {
            outBuf_.consume(static_cast<size_t>(w));
            continue;
        }
        if (w == -1) return; // 内核缓冲满，等下次可写
        handleClose("send error: " + Socket::lastSystemError());
        return;
    }
    enableWriteInterest(false);
    if (shutdownPending_) detail::shutdownWrite(fd_);
}

void TcpConnection::handleClose(const std::string& reason) {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return; // 幂等
    loop_->deregisterFd(fd_);
    if (onError_ && reason.rfind("peer closed") != 0) {
        onError_(shared_from_this(), reason);
    }
    if (onClose_) onClose_(shared_from_this());
    if (hostClose_) hostClose_(shared_from_this());
    sock_.close(); // 触发 Socket 析构路径统一关闭
}

} // namespace zenith::net
