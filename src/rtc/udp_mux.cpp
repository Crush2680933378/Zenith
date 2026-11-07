// ============================================================================
//  UDP demux 实现
// ============================================================================
#include "zenith/rtc/udp_mux.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <atomic>

namespace zenith::rtc {

DatagramKind classifyDatagram(const char* data, size_t n) {
    if (data == nullptr || n == 0) return DatagramKind::Unknown;
    const auto first = static_cast<uint8_t>(data[0]);
    if (first <= 3) return DatagramKind::Stun;                   // STUN/TURN 消息类型高 2 位为 00
    if (first >= 20 && first <= 63) return DatagramKind::Dtls;   // TLS record 20..63
    if (first >= 128 && first <= 191) return DatagramKind::Rtp;  // RTP 版本位 10xxxxxx
    return DatagramKind::Unknown;
}

struct UdpMux::Impl {
    int fd = -1;
    uint16_t port = 0;
    Handler stun;
    Handler dtls;
    Handler rtp;
    Handler unknown;
    std::atomic<size_t> dropped{0};

    static void closeSock(int s) {
        if (s < 0) return;
#if defined(_WIN32)
        closesocket(s);
#else
        ::close(s);
#endif
    }
};

UdpMux::UdpMux() : impl_(std::make_unique<Impl>()) {}

UdpMux::~UdpMux() {
    if (impl_->fd >= 0) Impl::closeSock(impl_->fd);
}

bool UdpMux::bind(uint16_t port) {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    impl_->fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (impl_->fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::bind(impl_->fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        Impl::closeSock(impl_->fd);
        impl_->fd = -1;
        return false;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(impl_->fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        impl_->port = ntohs(addr.sin_port);
    }
    return true;
}

uint16_t UdpMux::localPort() const {
    return impl_->port;
}
int UdpMux::socketFd() const {
    return impl_->fd;
}
size_t UdpMux::dropped() const {
    return impl_->dropped.load();
}

void UdpMux::onStun(Handler h) {
    impl_->stun = std::move(h);
}
void UdpMux::onDtls(Handler h) {
    impl_->dtls = std::move(h);
}
void UdpMux::onRtp(Handler h) {
    impl_->rtp = std::move(h);
}
void UdpMux::onUnknown(Handler h) {
    impl_->unknown = std::move(h);
}

int UdpMux::pollOnce(int timeoutMs) {
    if (impl_->fd < 0) return 0;

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(impl_->fd, &rd);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    if (::select(impl_->fd + 1, &rd, nullptr, nullptr, &tv) <= 0) return 0;

    char buf[2048];
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    const int n =
        ::recvfrom(impl_->fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (n <= 0) return 0;

    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    const StunAddress from{ip, static_cast<uint16_t>(ntohs(peer.sin_port))};
    const std::string_view payload(buf, static_cast<size_t>(n));

    switch (classifyDatagram(buf, static_cast<size_t>(n))) {
        case DatagramKind::Stun:
            if (impl_->stun) {
                impl_->stun(payload, from);
                return 1;
            }
            break;
        case DatagramKind::Dtls:
            if (impl_->dtls) {
                impl_->dtls(payload, from);
                return 1;
            }
            break;
        case DatagramKind::Rtp:
            if (impl_->rtp) {
                impl_->rtp(payload, from);
                return 1;
            }
            break;
        case DatagramKind::Unknown:
            break;
    }
    if (impl_->unknown) {
        impl_->unknown(payload, from);
        return 1;
    }
    impl_->dropped.fetch_add(1);
    return 0;
}

bool UdpMux::sendTo(const StunAddress& to, std::string_view data) {
    if (impl_->fd < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    if (::inet_pton(AF_INET, to.ip.c_str(), &addr.sin_addr) != 1) return false;
    const int n = ::sendto(impl_->fd, data.data(), static_cast<int>(data.size()), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(data.size());
}

}  // namespace zenith::rtc
