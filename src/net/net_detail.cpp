#include "net_detail.hpp"

#include <cstring>

#include "zenith/core/platform.hpp"

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
  using NativeSocket = SOCKET;
#else
  #include <fcntl.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using NativeSocket = int;
#endif

namespace zenith::net::detail {

fd_t createWakeupFd() {
    NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, 0);
#if defined(ZENITH_PLATFORM_WINDOWS)
    if (s == INVALID_SOCKET) return kInvalidFd;
    // Windows：绑定临时端口，向自身发 UDP
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return kInvalidFd;
    }
    sockaddr_in self{};
    socklen_t len = sizeof(self);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&self), &len) != 0) {
        ::closesocket(s);
        return kInvalidFd;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&self), sizeof(self)) != 0) {
        ::closesocket(s);
        return kInvalidFd;
    }
    return static_cast<fd_t>(s);
#else
    if (s < 0) return kInvalidFd;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::close(s);
        return kInvalidFd;
    }
    sockaddr_in self{};
    socklen_t len = sizeof(self);
    if (::getsockname(s, reinterpret_cast<sockaddr*>(&self), &len) != 0) {
        ::close(s);
        return kInvalidFd;
    }
    if (::connect(s, reinterpret_cast<sockaddr*>(&self), sizeof(self)) != 0) {
        ::close(s);
        return kInvalidFd;
    }
    return static_cast<fd_t>(s);
#endif
}

void closeRaw(fd_t fd) {
    if (fd == kInvalidFd) return;
#if defined(ZENITH_PLATFORM_WINDOWS)
    ::closesocket(static_cast<NativeSocket>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

int rawRead(fd_t fd, void* buf, size_t n) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    const int r = ::recv(static_cast<NativeSocket>(fd), static_cast<char*>(buf),
                         static_cast<int>(n), 0);
#else
    const int r = ::read(static_cast<int>(fd), buf, n);
#endif
    return r;
}

int rawWrite(fd_t fd, const void* buf, size_t n) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    return static_cast<int>(::send(static_cast<NativeSocket>(fd),
                                   static_cast<const char*>(buf), static_cast<int>(n), 0));
#else
    return static_cast<int>(::write(static_cast<int>(fd), buf, n));
#endif
}

void setFdNonBlocking(fd_t fd, bool on) {
    if (fd == kInvalidFd) return;
#if defined(ZENITH_PLATFORM_WINDOWS)
    u_long mode = on ? 1 : 0;
    ::ioctlsocket(static_cast<NativeSocket>(fd), FIONBIO, &mode);
#else
    const int flags = ::fcntl(static_cast<int>(fd), F_GETFL, 0);
    ::fcntl(static_cast<int>(fd), F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

void shutdownWrite(fd_t fd) {
    if (fd == kInvalidFd) return;
#if defined(ZENITH_PLATFORM_WINDOWS)
    ::shutdown(static_cast<NativeSocket>(fd), SD_SEND);
#else
    ::shutdown(static_cast<int>(fd), SHUT_WR);
#endif
}

int getSocketError(fd_t fd) {
    if (fd == kInvalidFd) return -1;
    int err = 0;
    socklen_t len = sizeof(err);
    if (::getsockopt(static_cast<NativeSocket>(fd), SOL_SOCKET, SO_ERROR,
                     reinterpret_cast<char*>(&err), &len) != 0) {
        return -1;
    }
    return err;
}

} // namespace zenith::net::detail
