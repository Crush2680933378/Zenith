// ============================================================================
//  zenith::net::Socket —— 跨平台 TCP socket 封装（Windows winsock2 / POSIX）
//  fd 统一为 fd_t(intptr_t)；kInvalidFd 表示无效。winsock 仅在 .cpp 内引入。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::net {

using fd_t = intptr_t;
constexpr fd_t kInvalidFd = -1;

struct Endpoint {
    std::string host{"0.0.0.0"};
    uint16_t    port = 0;
};

class ZENITH_API Socket {
public:
    Socket() = default;
    explicit Socket(fd_t fd) : fd_(fd) {}
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;
    ~Socket();

    /// 创建非阻塞 TCP socket（IPv4）。失败返回 Err。
    static core::Result<Socket> createTcp();

    bool valid() const noexcept { return fd_ != kInvalidFd; }
    [[nodiscard]] fd_t fd() const noexcept { return fd_; }
    /// 放弃所有权（不关闭），fd 移交给接管方。
    fd_t releaseFd() noexcept {
        const fd_t f = fd_;
        fd_ = kInvalidFd;
        return f;
    }

    core::Result<void> bind(const std::string& host, uint16_t port);
    core::Result<void> listen(int backlog = 128);
    /// 返回 accepted fd；EAGAIN 时返回 kInvalidFd 且 lastError 为空。
    core::Result<fd_t> accept(Endpoint* peerOut = nullptr);
    /// 非阻塞连接：立即成功 / 进行中(EINPROGRESS,WOULDBLOCK) / 失败。
    core::Result<bool> connect(const std::string& host, uint16_t port);

    /// bind(临时端口 0) 后查询实际端口。
    core::Result<uint16_t> localPort() const;

    [[nodiscard]] Endpoint peerEndpoint() const;

    void setNonBlocking(bool on);
    void setNoDelay(bool on);
    void setReuseAddr(bool on);
    void setKeepAlive(bool on);

    /// 返回 >0 字节数；0 对端关闭；-1 EAGAIN/无数据；-2 其它错误。
    int readSome(char* buf, size_t cap);
    /// 返回 >=0 写出字节数；-1 EAGAIN；-2 其它错误。
    int writeSome(const char* buf, size_t n);

    void close();

    /// 当前系统错误描述（跨平台 GetLastError/errno）。
    [[nodiscard]] static std::string lastSystemError();

private:
    fd_t fd_ = kInvalidFd;
};

/// 阻塞式 DNS/地址解析（getaddrinfo，仅 IPv4）。
/// M2 说明：异步 DNS 留待后续里程碑；服务端 accept 路径不经过它。
ZENITH_API core::Result<uint32_t> resolveHostIpv4(const std::string& host);

} // namespace zenith::net
