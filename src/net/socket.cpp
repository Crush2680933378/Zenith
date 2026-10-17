#include "zenith/net/socket.hpp"

#include <cstring>

#include "zenith/core/platform.hpp"

#if !defined(ZENITH_PLATFORM_WINDOWS)
  #include <csignal> // SIGPIPE 忽略（见下方 SigpipeGuard）
#endif

#if defined(ZENITH_PLATFORM_WINDOWS)
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

namespace zenith::net {

#if defined(ZENITH_PLATFORM_WINDOWS)

namespace {
// 进程级 WSAStartup 守卫（首次构造网络对象时初始化 winsock）。
struct WsaGuard {
    WsaGuard() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
const WsaGuard g_wsaGuard;
} // namespace

using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidNative = INVALID_SOCKET;

inline int sockErr() noexcept { return WSAGetLastError(); }
constexpr bool wouldBlock(int e) noexcept { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
inline void closeNative(fd_t fd) noexcept {
    if (fd != kInvalidFd) closesocket(static_cast<NativeSocket>(fd));
}

#else

using NativeSocket = int;
constexpr NativeSocket kInvalidNative = -1;

namespace {
// Linux 写入已关闭对端的 socket 触发 SIGPIPE 默认终止进程——服务端必须忽略
// （Windows 对应 WSAECONNRESET，由错误路径处理）。首次创建 socket 时生效一次。
struct SigpipeGuard {
    SigpipeGuard() { std::signal(SIGPIPE, SIG_IGN); }
};
const SigpipeGuard g_sigpipeGuard;
} // namespace

inline int sockErr() noexcept { return errno; }
constexpr bool wouldBlock(int e) noexcept { return e == EAGAIN || e == EWOULDBLOCK || e == EINPROGRESS; }
inline void closeNative(fd_t fd) noexcept {
    if (fd != kInvalidFd) ::close(static_cast<int>(fd));
}

#endif

std::string Socket::lastSystemError() {
    const int e = sockErr();
#if defined(ZENITH_PLATFORM_WINDOWS)
    char buf[256];
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, static_cast<DWORD>(e),
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     buf, sizeof(buf), nullptr);
    return "wsa error " + std::to_string(e) + ": " +
           (len > 0 ? std::string(buf, len) : std::string("unknown"));
#else
    return std::string("errno ") + std::to_string(e) + ": " + std::strerror(e);
#endif
}

Socket::Socket(Socket&& other) noexcept : fd_(other.fd_) { other.fd_ = kInvalidFd; }

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, kInvalidFd);
    }
    return *this;
}

Socket::~Socket() { close(); }

core::Result<Socket> Socket::createTcp() {
#if defined(ZENITH_PLATFORM_WINDOWS)
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidNative) {
        return core::Result<Socket>::Err(core::Error(10, "socket() failed: " + lastSystemError()));
    }
    Socket sock(static_cast<fd_t>(s));
    sock.setNonBlocking(true); // Reactor 一律非阻塞
    return core::Result<Socket>::Ok(std::move(sock));
#else
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return core::Result<Socket>::Err(core::Error(10, "socket() failed: " + lastSystemError()));
    }
    Socket sock(static_cast<fd_t>(s));
    sock.setNonBlocking(true);
    return core::Result<Socket>::Ok(std::move(sock));
#endif
}

core::Result<uint32_t> resolveHostIpv4(const std::string& host) {
    // 通配符：监听场景
    if (host.empty() || host == "0.0.0.0" || host == "*" || host == "::") {
        return core::Result<uint32_t>::Ok(htonl(INADDR_ANY));
    }
    // 数字地址快速路径（免 DNS）
    in_addr numeric{};
    if (::inet_pton(AF_INET, host.c_str(), &numeric) == 1) {
        return core::Result<uint32_t>::Ok(numeric.s_addr);
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const int rc = ::getaddrinfo(host.c_str(), nullptr, &hints, &res);
    if (rc != 0 || res == nullptr) {
        return core::Result<uint32_t>::Err(
            core::Error(11, "resolve failed for '" + host + "': " +
#if defined(ZENITH_PLATFORM_WINDOWS)
                                std::to_string(rc)));
#else
                                gai_strerror(rc)));
#endif
    }
    uint32_t ip = 0;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        if (p->ai_family == AF_INET) {
            ip = reinterpret_cast<sockaddr_in*>(p->ai_addr)->sin_addr.s_addr;
            break;
        }
    }
    ::freeaddrinfo(res);
    if (ip == 0) {
        return core::Result<uint32_t>::Err(
            core::Error(11, "no IPv4 address for '" + host + "'"));
    }
    return core::Result<uint32_t>::Ok(ip);
}

core::Result<void> Socket::bind(const std::string& host, uint16_t port) {
    if (!valid()) return core::Result<void>::Err(core::Error(12, "bind: invalid socket"));
    auto ip = resolveHostIpv4(host.empty() ? std::string("0.0.0.0") : host);
    if (!ip) return core::Result<void>::Err(std::move(ip).error());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip.value();
    if (::bind(static_cast<NativeSocket>(fd_), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return core::Result<void>::Err(core::Error(12, "bind failed: " + lastSystemError()));
    }
    return core::Result<void>::Ok();
}

core::Result<void> Socket::listen(int backlog) {
    if (!valid()) return core::Result<void>::Err(core::Error(13, "listen: invalid socket"));
    if (::listen(static_cast<NativeSocket>(fd_), backlog) != 0) {
        return core::Result<void>::Err(core::Error(13, "listen failed: " + lastSystemError()));
    }
    return core::Result<void>::Ok();
}

core::Result<fd_t> Socket::accept(Endpoint* peerOut) {
    if (!valid()) return core::Result<fd_t>::Err(core::Error(14, "accept: invalid socket"));
    sockaddr_in peer{};
    socklen_t len = sizeof(peer);
    const NativeSocket c =
        ::accept(static_cast<NativeSocket>(fd_), reinterpret_cast<sockaddr*>(&peer), &len);
    if (c == kInvalidNative) {
        const int e = sockErr();
        if (wouldBlock(e)) return core::Result<fd_t>::Ok(kInvalidFd);
        return core::Result<fd_t>::Err(core::Error(14, "accept failed: " + lastSystemError()));
    }
    if (peerOut != nullptr) {
        char buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &peer.sin_addr, buf, sizeof(buf));
        peerOut->host = buf;
        peerOut->port = ntohs(peer.sin_port);
    }
#if defined(ZENITH_PLATFORM_WINDOWS)
    // accepted socket 继承 non-blocking？Windows 上监听 socket 的属性不继承，需显式设置
    u_long mode = 1;
    ::ioctlsocket(c, FIONBIO, &mode);
#else
    const int flags = ::fcntl(c, F_GETFL, 0);
    ::fcntl(c, F_SETFL, flags | O_NONBLOCK);
#endif
    return core::Result<fd_t>::Ok(static_cast<fd_t>(c));
}

core::Result<bool> Socket::connect(const std::string& host, uint16_t port) {
    if (!valid()) return core::Result<bool>::Err(core::Error(15, "connect: invalid socket"));
    auto ip = resolveHostIpv4(host);
    if (!ip) return core::Result<bool>::Err(std::move(ip).error());

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip.value();
    const int rc = ::connect(static_cast<NativeSocket>(fd_),
                             reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) return core::Result<bool>::Ok(true); // 立即成功（如 loopback）
    const int e = sockErr();
    if (wouldBlock(e)) return core::Result<bool>::Ok(false); // 进行中
    return core::Result<bool>::Err(core::Error(15, "connect failed: " + lastSystemError()));
}

core::Result<uint16_t> Socket::localPort() const {
    if (!valid()) return core::Result<uint16_t>::Err(core::Error(16, "localPort: invalid socket"));
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(static_cast<NativeSocket>(fd_), reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        return core::Result<uint16_t>::Err(core::Error(16, "getsockname failed: " + lastSystemError()));
    }
    return core::Result<uint16_t>::Ok(ntohs(addr.sin_port));
}

Endpoint Socket::peerEndpoint() const {
    Endpoint ep;
    if (!valid()) return ep;
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getpeername(static_cast<NativeSocket>(fd_), reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        char buf[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, &addr.sin_addr, buf, sizeof(buf));
        ep.host = buf;
        ep.port = ntohs(addr.sin_port);
    }
    return ep;
}

void Socket::setNonBlocking(bool on) {
    if (!valid()) return;
#if defined(ZENITH_PLATFORM_WINDOWS)
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(static_cast<NativeSocket>(fd_), FIONBIO, &mode);
#else
    const int flags = ::fcntl(static_cast<int>(fd_), F_GETFL, 0);
    ::fcntl(static_cast<int>(fd_), F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void Socket::setNoDelay(bool on) {
    if (!valid()) return;
    int v = on ? 1 : 0;
    ::setsockopt(static_cast<NativeSocket>(fd_), IPPROTO_TCP, TCP_NODELAY,
                 reinterpret_cast<const char*>(&v), sizeof(v));
}

void Socket::setReuseAddr(bool on) {
    if (!valid()) return;
    int v = on ? 1 : 0;
    ::setsockopt(static_cast<NativeSocket>(fd_), SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&v), sizeof(v));
}

void Socket::setKeepAlive(bool on) {
    if (!valid()) return;
    int v = on ? 1 : 0;
    ::setsockopt(static_cast<NativeSocket>(fd_), SOL_SOCKET, SO_KEEPALIVE,
                 reinterpret_cast<const char*>(&v), sizeof(v));
}

int Socket::readSome(char* buf, size_t cap) {
    const int n = ::recv(static_cast<NativeSocket>(fd_), buf, static_cast<int>(cap), 0);
    if (n > 0) return n;
    if (n == 0) return 0; // 对端有序关闭
    const int e = sockErr();
    if (wouldBlock(e)) return -1;
    return -2;
}

int Socket::writeSome(const char* buf, size_t n) {
    const int w = ::send(static_cast<NativeSocket>(fd_), buf, static_cast<int>(n), 0);
    if (w >= 0) return w;
    const int e = sockErr();
    if (wouldBlock(e)) return -1;
    return -2;
}

void Socket::close() {
    if (fd_ != kInvalidFd) {
        closeNative(fd_);
        fd_ = kInvalidFd;
    }
}

} // namespace zenith::net
