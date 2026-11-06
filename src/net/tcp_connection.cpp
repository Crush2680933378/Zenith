#include "zenith/net/tcp_connection.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <utility>

#include "net_detail.hpp"
#include "zenith/core/platform.hpp"
#include "zenith/tls/tls_context.hpp"

#if defined(ZENITH_PLATFORM_LINUX)
#include <sys/sendfile.h>
#include <unistd.h>
#elif defined(ZENITH_PLATFORM_MACOS)
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>
#endif

namespace zenith::net {

namespace {
/// 以二进制方式打开待发送文件（Windows 下走宽字符路径，非 ASCII 路径可打开）
std::FILE* openForSend(const std::string& path) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    const std::wstring wide = std::filesystem::path(path).wstring();
    return _wfopen(wide.c_str(), L"rb");
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

/// 64 位定位（Windows 的 long 只有 32 位，必须用 _fseeki64）
bool seekFile(std::FILE* f, uint64_t offset) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    return _fseeki64(f, static_cast<__int64>(offset), SEEK_SET) == 0;
#else
    return fseeko(f, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}
}  // namespace

TcpConnection::TcpConnection(EventLoop* loop, fd_t fd, std::string peer)
    : loop_(loop), fd_(fd), peer_(std::move(peer)), sock_(fd) {}

TcpConnection::~TcpConnection() {
    // fd_ 由 Socket 析构统一关闭；deregister 由 handleClose 完成
    closeFileSend();
}

void TcpConnection::establish() {
    connected_.store(true, std::memory_order_release);
    auto self = shared_from_this();
    loop_->registerFd(fd_, kPollReadable,
                      [self](uint16_t revents) { self->handleEvents(revents); });
    if (tls_ != nullptr) {
        // TLS 连接：onConnect 推迟到握手完成（handleTls 推进）
        std::string wire;
        tls_->encrypt(nullptr, 0, wire);  // 触发客户端握手泵
        if (!wire.empty()) sockSend(wire);
        return;
    }
    if (onConnect_) onConnect_(self);
}

void TcpConnection::startTls(std::unique_ptr<tls::TlsFilter> filter) {
    tls_ = std::move(filter);
    if (!connected_.load(std::memory_order_acquire)) return;  // establish 时接管
    std::string wire;
    tls_->encrypt(nullptr, 0, wire);
    if (!wire.empty()) sockSend(wire);
}

bool TcpConnection::tlsEstablished() const noexcept {
    return tls_ != nullptr && tls_->state() == tls::TlsState::Established;
}

/// 底层密文直发（绕过 TLS 层）
void TcpConnection::sockSend(const std::string& wire) {
    if (wire.empty()) return;
    if (writing_ || outBuf_.readableBytes() > 0) {
        outBuf_.append(wire.data(), wire.size());
        enableWriteInterest(true);
        return;
    }
    const int w = sock_.writeSome(wire.data(), wire.size());
    size_t remaining = wire.size();
    if (w > 0) remaining = wire.size() - static_cast<size_t>(w);
    if (remaining > 0) {
        outBuf_.append(wire.data() + (wire.size() - remaining), remaining);
        enableWriteInterest(true);
    } else if (w == -2) {
        handleClose("send error: " + Socket::lastSystemError());
    }
}

/// TLS 泵：处理握手推进/解密；返回 false 表示 TLS 失败需关连接
bool TcpConnection::pumpTls(const char* cipher, size_t n) {
    std::string plain, wire;
    tls_->feedCipher(cipher, n, plain, wire);
    if (!wire.empty()) sockSend(wire);
    if (tls_->state() == tls::TlsState::Failed) {
        handleClose("tls error: " + tls_->error());
        return false;
    }
    if (!plain.empty()) inBuf_.append(plain.data(), plain.size());
    if (tls_->consumeHandshakeDone() && onConnect_) onConnect_(shared_from_this());
    return true;
}

void TcpConnection::finalize(CloseCallback hostClose) {
    hostClose_ = std::move(hostClose);
}

void TcpConnection::send(const void* data, size_t n) {
    if (!connected_.load(std::memory_order_acquire) || n == 0) return;
    auto self = shared_from_this();
    loop_->runInLoop([self, d = std::string(static_cast<const char*>(data), n)] {
        self->sendInLoop(d.data(), d.size());
    });
}

void TcpConnection::sendInLoop(const void* data, size_t n) {
    if (!connected_.load(std::memory_order_acquire)) return;
    if (tls_ != nullptr) {
        // 明文先入 TLS 层；密文（含握手期暂存）走底层发送
        std::string wire;
        tls_->encrypt(static_cast<const char*>(data), n, wire);
        if (!wire.empty()) sockSend(wire);
        if (tls_->state() == tls::TlsState::Failed) {
            handleClose("tls error: " + tls_->error());
        }
        return;
    }
    if (writing_ || fileSendActive() || outBuf_.readableBytes() > 0) {
        // 文件正文在途：新的 send() 必须排到文件之后（postFileBuf_），
        // 否则字节会插进文件内容中间。文件发完由 handleWrite 接上。
        if (fileSendActive()) {
            if (tls_ != nullptr) {
                std::string wire;
                tls_->encrypt(static_cast<const char*>(data), n, wire);
                if (!wire.empty()) postFileBuf_.append(wire.data(), wire.size());
            } else {
                postFileBuf_.append(data, n);
            }
        } else {
            outBuf_.append(data, n);
        }
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
    if (shutdownPending_ && outBuf_.readableBytes() == 0 && !fileSendActive()) {
        // 已排空且收到 shutdown 请求 → 真正半关闭
        detail::shutdownWrite(fd_);
    }
}

void TcpConnection::shutdown() {
    auto self = shared_from_this();
    loop_->runInLoop([self] {
        if (!self->connected_.load()) return;
        // 文件正文在途时也必须等它发完再半关闭（否则客户端收不全）
        if (self->outBuf_.readableBytes() == 0 && self->postFileBuf_.readableBytes() == 0 &&
            !self->fileSendActive()) {
            detail::shutdownWrite(self->fd_);
        } else {
            self->shutdownPending_ = true;  // 排空后再关
        }
    });
}

/* ============================ 文件零拷贝发送（M7.3） ============================ */

void TcpConnection::sendFile(const std::string& path, uint64_t offset, uint64_t count) {
    if (count == 0) return;
    auto self = shared_from_this();
    loop_->runInLoop([self, path, offset, count] { self->startFileSend(path, offset, count); });
}

void TcpConnection::startFileSend(std::string path, uint64_t offset, uint64_t count) {
    if (!connected_.load(std::memory_order_acquire) || count == 0) return;
    closeFileSend();  // 防御：正常流程下不会有残留

    std::FILE* f = openForSend(path);
    if (f == nullptr) {
        handleClose("sendfile open failed: " + path);
        return;
    }
    if (!seekFile(f, offset)) {
        std::fclose(f);
        handleClose("sendfile seek failed: " + path);
        return;
    }
    file_ = f;
    fileRemain_ = count;
    fileOffset_ = offset;

    // 响应头可能仍在 outBuf_（大响应头/内核满）：必须先完全排空再发正文，
    // 否则头部残字节会插进文件内容中间。
    if (outBuf_.readableBytes() > 0) {
        filePendingStart_ = true;
        enableWriteInterest(true);
        return;
    }
    filePendingStart_ = false;
    if (pumpFileSend()) enableWriteInterest(true);
}

bool TcpConnection::pumpFileSend() {
    if (file_ == nullptr) return false;

#if defined(ZENITH_PLATFORM_LINUX) || defined(ZENITH_PLATFORM_MACOS)
    // TLS 连接：sendfile 直接把明文写进 socket 会破坏加密流，必须走回退路径
    if (tls_ == nullptr) {
        const int ffd = fileno(file_);  // NOLINT：sendfile 需要裸 fd
#if defined(ZENITH_PLATFORM_LINUX)
        while (fileRemain_ > 0) {
            off_t off = static_cast<off_t>(fileOffset_);
            const size_t chunk = static_cast<size_t>(std::min<uint64_t>(fileRemain_, 1u << 20));
            const ssize_t n = ::sendfile(static_cast<int>(fd_), ffd, &off, chunk);
            if (n > 0) {
                fileOffset_ = static_cast<uint64_t>(off);
                fileRemain_ -= static_cast<uint64_t>(n);
                continue;
            }
            if (n == 0) {
                fileRemain_ = 0;
                break;
            }  // 文件短于声明的长度
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return true;  // 等可写
            handleClose("sendfile error: " + Socket::lastSystemError());
            closeFileSend();
            return false;
        }
#else  // macOS：sendfile(fileFd, sockFd, offset, &len, hdtr, flags)
        while (fileRemain_ > 0) {
            off_t len = static_cast<off_t>(std::min<uint64_t>(fileRemain_, 1u << 20));
            const int rc = ::sendfile(ffd, static_cast<int>(fd_), static_cast<off_t>(fileOffset_),
                                      &len, nullptr, 0);
            if (rc == 0) {
                fileOffset_ += static_cast<uint64_t>(len);
                fileRemain_ -= static_cast<uint64_t>(len);
                if (len == 0) {
                    fileRemain_ = 0;
                    break;
                }
                continue;
            }
            if (errno == EAGAIN) {  // 部分发送：len 为实际发出字节
                fileOffset_ += static_cast<uint64_t>(len);
                fileRemain_ -= static_cast<uint64_t>(len);
                return true;
            }
            if (errno == EINTR) continue;
            handleClose("sendfile error: " + Socket::lastSystemError());
            closeFileSend();
            return false;
        }
#endif
        closeFileSend();
        return false;
    }
#endif
    // 回退路径（Windows / TLS）：分块读发，内存占用有界（64KB）。
    //
    // 这里**直接写 socket**，不再绕 sendInLoop：sendInLoop 在文件在途时会把数据
    // 塞进 postFileBuf_（那是给「文件之后的响应」用的），绕进去既多一次拷贝，
    // 又会让 outBuf_ 只涨不排——实测大文件吞吐掉 20%，进程 RSS 涨到 168MB。
    char buf[65536];
    while (fileRemain_ > 0 && outBuf_.readableBytes() == 0) {
        const size_t want = static_cast<size_t>(std::min<uint64_t>(fileRemain_, sizeof(buf)));
        const size_t got = std::fread(buf, 1, want, file_);
        if (got == 0) {  // 提前 EOF（文件被截断）——按实际读到的字节收尾
            fileRemain_ = 0;
            break;
        }
        fileRemain_ -= got;

        const char* src = buf;
        size_t len = got;
        std::string wire;
        if (tls_ != nullptr) {  // TLS：先加密再写
            tls_->encrypt(buf, got, wire);
            if (tls_->state() == tls::TlsState::Failed) {
                handleClose("tls error: " + tls_->error());
                closeFileSend();
                return false;
            }
            src = wire.data();
            len = wire.size();
            if (len == 0) continue;
        }
        const int w = sock_.writeSome(src, len);
        if (w > 0 && static_cast<size_t>(w) < len) {
            outBuf_.append(src + w, len - static_cast<size_t>(w));  // 尾部留待下轮
            break;
        }
        if (w == -1) {  // 内核满：整块留下，等可写
            outBuf_.append(src, len);
            break;
        }
        if (w == -2) {
            handleClose("send error: " + Socket::lastSystemError());
            closeFileSend();
            return false;
        }
    }
    if (fileRemain_ == 0) {
        closeFileSend();
        return false;
    }
    return true;
}

void TcpConnection::closeFileSend() noexcept {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
    fileRemain_ = 0;
    filePendingStart_ = false;
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
            if (tls_ != nullptr) {
                if (!pumpTls(buf, static_cast<size_t>(n))) return;  // TLS 失败已关
            } else {
                inBuf_.append(buf, static_cast<size_t>(n));
            }
            if (n < static_cast<int>(sizeof(buf))) break;  // 读尽本轮
            continue;
        }
        if (n == -1) break;  // EAGAIN
        if (n == 0) {        // 对端关闭
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
    for (;;) {
        // 1) 先排空 outBuf_（响应头，或上一轮文件分块没写完的尾巴）
        bool blocked = false;
        while (outBuf_.readableBytes() > 0) {
            const int w = sock_.writeSome(reinterpret_cast<const char*>(outBuf_.peek()),
                                          outBuf_.readableBytes());
            if (w > 0) {
                outBuf_.consume(static_cast<size_t>(w));
                continue;
            }
            if (w == -1) {  // 内核缓冲满，等下次可写
                blocked = true;
                break;
            }
            handleClose("send error: " + Socket::lastSystemError());
            return;
        }
        if (blocked) return;

        // 2) 头部已排空 → 启动此前排队的文件正文（M7.3）
        if (filePendingStart_ && file_ != nullptr) {
            filePendingStart_ = false;
            continue;
        }
        // 3) 泵文件正文：它必须紧跟头部，且不走 postFileBuf_（那是文件之后的字节）
        if (file_ != nullptr && !filePendingStart_) {
            if (pumpFileSend()) return;  // 内核或文件读满，等下一次可写
            continue;                    // 文件发完 → 去接文件期间排队的响应
        }
        // 4) 文件发完：把期间 send() 排到 postFileBuf_ 的字节接上
        if (postFileBuf_.readableBytes() > 0) {
            outBuf_.append(postFileBuf_.peek(), postFileBuf_.readableBytes());
            postFileBuf_.retrieveAll();
            continue;
        }
        break;
    }
    enableWriteInterest(false);
    // M7.2c：发送排空后空闲缩容（256KB 阈值——小缓冲不折腾）
    outBuf_.shrinkIfBloated(256 * 1024);
    if (shutdownPending_) detail::shutdownWrite(fd_);
}

void TcpConnection::handleClose(const std::string& reason) {
    if (!connected_.exchange(false, std::memory_order_acq_rel)) return;  // 幂等
    closeFileSend();
    // TLS：尽力发送 close_notify（连接即将关闭，一次直写不重试）
    if (tls_ != nullptr) {
        std::string wire;
        tls_->shutdown(wire);
        if (!wire.empty()) (void)sock_.writeSome(wire.data(), wire.size());
    }
    loop_->deregisterFd(fd_);
    if (onError_ && reason.rfind("peer closed") != 0) {
        onError_(shared_from_this(), reason);
    }
    if (onClose_) onClose_(shared_from_this());
    if (hostClose_) hostClose_(shared_from_this());
    // 断开所有回调：它们常捕获上层对象（WebSocketSession、HttpClient 的
    // AsyncState），而对方又持有本连接 → 关闭后整组对象互相吊着，
    // 进程退出时仍不释放（LSan 报的 TcpConnection 缓冲泄漏即此类）。
    //
    // 必须投递到本轮事件之后再清：当前调用栈往往就在 onMessage_ 的闭包里，
    // 同步置空会析构正在执行的函数对象（UAF，M3 5d 规则）。
    auto self = shared_from_this();
    loop_->queueInLoop([self] {
        self->onConnect_ = nullptr;
        self->onMessage_ = nullptr;
        self->onClose_ = nullptr;
        self->onError_ = nullptr;
        self->hostClose_ = nullptr;
    });
    sock_.close();  // 触发 Socket 析构路径统一关闭
}

}  // namespace zenith::net
