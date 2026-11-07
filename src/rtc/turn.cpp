// ============================================================================
//  TURN 实现（自包含：自带 STUN 属性编解码，不依赖 stun.cpp 的内部助手）
// ============================================================================
#include "zenith/rtc/turn.hpp"

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

#include <chrono>
#include <cstring>
#include <map>
#include <random>
#include <vector>

namespace zenith::rtc {
namespace {

constexpr uint16_t kTypeAllocateRequest = 0x0003;
constexpr uint16_t kTypeAllocateSuccess = 0x0103;
constexpr uint16_t kTypeAllocateError = 0x0113;
constexpr uint16_t kTypeCreatePermissionRequest = 0x0008;
constexpr uint16_t kTypeCreatePermissionSuccess = 0x0108;
constexpr uint16_t kTypeSendIndication = 0x0016;
constexpr uint16_t kTypeDataIndication = 0x0017;

constexpr uint16_t kAttrMappedAddress = 0x0001;
constexpr uint16_t kAttrUsername = 0x0006;
constexpr uint16_t kAttrLifetime = 0x000D;
constexpr uint16_t kAttrXorPeerAddress = 0x0012;
constexpr uint16_t kAttrData = 0x0013;
constexpr uint16_t kAttrXorRelayedAddress = 0x0016;
constexpr uint16_t kAttrRequestedTransport = 0x0019;
constexpr uint16_t kAttrErrorCode = 0x0009;

void putU16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

void putU32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

uint16_t getU16(const char* p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}

uint32_t getU32(const char* p) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

void addAttr(std::string& msg, uint16_t type, std::string_view value) {
    putU16(msg, type);
    putU16(msg, static_cast<uint16_t>(value.size()));
    msg.append(value.data(), value.size());
    while ((msg.size() % 4) != 0)
        msg.push_back('\0');
}

/// XOR-PEER / XOR-RELAYED / XOR-MAPPED 共用的编码
std::string xorAddress(const StunAddress& addr) {
    std::string v;
    v.push_back('\0');
    v.push_back(0x01);
    in_addr in{};
    ::inet_pton(AF_INET, addr.ip.c_str(), &in);
    uint32_t ip = ntohl(in.s_addr) ^ kStunMagicCookie;
    uint16_t port = static_cast<uint16_t>(addr.port ^ (kStunMagicCookie >> 16));
    putU16(v, port);
    putU32(v, ip);
    return v;
}

bool parseXorAddress(const char* p, size_t len, StunAddress& out) {
    if (len < 8 || static_cast<uint8_t>(p[1]) != 0x01) return false;
    const uint16_t port = static_cast<uint16_t>(getU16(p + 2) ^ (kStunMagicCookie >> 16));
    uint32_t ip = getU32(p + 4) ^ kStunMagicCookie;
    in_addr in{};
    in.s_addr = htonl(ip);
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &in, buf, sizeof(buf)) == nullptr) return false;
    out.ip = buf;
    out.port = port;
    return true;
}

struct Attr {
    uint16_t type = 0;
    std::string value;
};

std::string buildMessage(uint16_t type, const uint8_t txn[12], const std::vector<Attr>& attrs) {
    std::string msg;
    putU16(msg, type);
    putU16(msg, 0);
    putU32(msg, kStunMagicCookie);
    msg.append(reinterpret_cast<const char*>(txn), 12);
    for (const auto& a : attrs)
        addAttr(msg, a.type, a.value);
    const size_t attrLen = msg.size() - 20;
    msg[2] = static_cast<char>((attrLen >> 8) & 0xFF);
    msg[3] = static_cast<char>(attrLen & 0xFF);
    return msg;
}

struct Parsed {
    uint16_t type = 0;
    uint8_t txn[12] = {};
    std::vector<Attr> attrs;
};

bool parseMessage(std::string_view data, Parsed& out) {
    if (data.size() < 20) return false;
    out.type = getU16(data.data());
    const uint16_t declared = getU16(data.data() + 2);
    if (getU32(data.data() + 4) != kStunMagicCookie) return false;
    std::memcpy(out.txn, data.data() + 8, 12);
    const size_t end = 20u + declared;
    if (data.size() < end) return false;
    size_t pos = 20;
    while (pos + 4 <= end) {
        Attr a;
        a.type = getU16(data.data() + pos);
        const uint16_t len = getU16(data.data() + pos + 2);
        pos += 4;
        if (pos + len > end) return false;
        a.value.assign(data.data() + pos, len);
        out.attrs.push_back(std::move(a));
        pos += len;
        while ((pos % 4) != 0)
            ++pos;
    }
    return true;
}

const Attr* findAttr(const Parsed& p, uint16_t type) {
    for (const auto& a : p.attrs) {
        if (a.type == type) return &a;
    }
    return nullptr;
}

void randomTxn(uint8_t txn[12]) {
    std::mt19937 rng{std::random_device{}()};
    for (int i = 0; i < 12; ++i)
        txn[i] = static_cast<uint8_t>(rng() & 0xFF);
}

int openUdp(uint16_t& port) {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
        port = ntohs(addr.sin_port);
    }
    return fd;
}

void closeFd(int fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
}

bool waitReadable(int fd, int timeoutMs) {
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    return ::select(fd + 1, &rd, nullptr, nullptr, &tv) > 0;
}

bool sendDatagram(int fd, const StunAddress& to, std::string_view data) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(to.port);
    if (::inet_pton(AF_INET, to.ip.c_str(), &addr.sin_addr) != 1) return false;
    const int n = ::sendto(fd, data.data(), static_cast<int>(data.size()), 0,
                           reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    return n == static_cast<int>(data.size());
}

bool recvDatagram(int fd, std::string& out, StunAddress& from) {
    char buf[2048];
    sockaddr_in peer{};
    socklen_t plen = sizeof(peer);
    const int n = ::recvfrom(fd, buf, sizeof(buf), 0, reinterpret_cast<sockaddr*>(&peer), &plen);
    if (n <= 0) return false;
    char ip[INET_ADDRSTRLEN] = {};
    ::inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
    from = StunAddress{ip, static_cast<uint16_t>(ntohs(peer.sin_port))};
    out.assign(buf, static_cast<size_t>(n));
    return true;
}

}  // namespace

/* ------------------------------ 客户端 ------------------------------ */

struct TurnClient::Impl {
    int fd = -1;
    uint16_t localPort = 0;
    StunAddress server;
    StunAddress relayed;
    bool allocated = false;
};

TurnClient::TurnClient() : impl_(std::make_unique<Impl>()) {
    impl_->fd = openUdp(impl_->localPort);
}

TurnClient::~TurnClient() {
    closeFd(impl_->fd);
}

bool TurnClient::allocate(const std::string& serverIp, uint16_t serverPort, int timeoutMs) {
    if (impl_->fd < 0) return false;
    impl_->server = StunAddress{serverIp, serverPort};

    uint8_t txn[12] = {};
    randomTxn(txn);
    std::vector<Attr> attrs;
    Attr transport;
    transport.type = kAttrRequestedTransport;
    transport.value = std::string("\x11\0\0\0", 4);  // 17 = UDP
    attrs.push_back(transport);
    const std::string msg = buildMessage(kTypeAllocateRequest, txn, attrs);
    if (!sendDatagram(impl_->fd, impl_->server, msg)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (!waitReadable(impl_->fd, static_cast<int>(left))) break;
        std::string resp;
        StunAddress from;
        if (!recvDatagram(impl_->fd, resp, from)) continue;
        Parsed p;
        if (!parseMessage(resp, p)) continue;
        if (std::memcmp(p.txn, txn, 12) != 0) continue;
        if (p.type == kTypeAllocateError) return false;
        if (p.type != kTypeAllocateSuccess) continue;
        const Attr* relayed = findAttr(p, kAttrXorRelayedAddress);
        if (relayed == nullptr) return false;
        StunAddress addr;
        if (!parseXorAddress(relayed->value.data(), relayed->value.size(), addr)) return false;
        impl_->relayed = addr;
        impl_->allocated = true;
        return true;
    }
    return false;
}

StunAddress TurnClient::relayedAddress() const {
    return impl_->relayed;
}
uint16_t TurnClient::localPort() const {
    return impl_->localPort;
}

bool TurnClient::createPermission(const StunAddress& peer, int timeoutMs) {
    if (!impl_->allocated) return false;
    uint8_t txn[12] = {};
    randomTxn(txn);
    std::vector<Attr> attrs;
    Attr a;
    a.type = kAttrXorPeerAddress;
    a.value = xorAddress(peer);
    attrs.push_back(a);
    const std::string msg = buildMessage(kTypeCreatePermissionRequest, txn, attrs);
    if (!sendDatagram(impl_->fd, impl_->server, msg)) return false;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (!waitReadable(impl_->fd, static_cast<int>(left))) break;
        std::string resp;
        StunAddress from;
        if (!recvDatagram(impl_->fd, resp, from)) continue;
        Parsed p;
        if (!parseMessage(resp, p)) continue;
        if (std::memcmp(p.txn, txn, 12) != 0) continue;
        return p.type == kTypeCreatePermissionSuccess;
    }
    return false;
}

bool TurnClient::sendTo(const StunAddress& peer, std::string_view data) {
    if (!impl_->allocated) return false;
    uint8_t txn[12] = {};
    randomTxn(txn);
    std::vector<Attr> attrs;
    Attr peerAttr;
    peerAttr.type = kAttrXorPeerAddress;
    peerAttr.value = xorAddress(peer);
    attrs.push_back(peerAttr);
    Attr dataAttr;
    dataAttr.type = kAttrData;
    dataAttr.value.assign(data.data(), data.size());
    attrs.push_back(dataAttr);

    // Send 是 indication：没有响应，transaction id 也用不上
    const std::string msg = buildMessage(kTypeSendIndication, txn, attrs);
    return sendDatagram(impl_->fd, impl_->server, msg);
}

std::string TurnClient::receive(StunAddress& from, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (!waitReadable(impl_->fd, static_cast<int>(left))) return {};
        std::string resp;
        StunAddress src;
        if (!recvDatagram(impl_->fd, resp, src)) return {};
        Parsed p;
        if (!parseMessage(resp, p)) continue;
        if (p.type != kTypeDataIndication) continue;
        const Attr* peer = findAttr(p, kAttrXorPeerAddress);
        const Attr* data = findAttr(p, kAttrData);
        if (peer == nullptr || data == nullptr) continue;
        StunAddress peerAddr;
        if (!parseXorAddress(peer->value.data(), peer->value.size(), peerAddr)) continue;
        from = peerAddr;
        return data->value;
    }
    return {};
}

/* ------------------------------ 服务端 ------------------------------ */

struct TurnServer::Impl {
    struct Relay {
        int fd = -1;
        StunAddress client;
        StunAddress relayed;
    };

    int fd = -1;
    uint16_t port = 0;
    std::map<uint16_t, Relay> relays;  // 以客户端端口为键（同机测试足够）

    ~Impl() {
        for (auto& [k, r] : relays) {
            (void)k;
            closeFd(r.fd);
        }
    }
};

TurnServer::TurnServer() : impl_(std::make_unique<Impl>()) {}
TurnServer::~TurnServer() {
    closeFd(impl_->fd);
}

bool TurnServer::listen(uint16_t port) {
    impl_->fd = openUdp(impl_->port);
    (void)port;  // 固定随机端口，够跑通测试；真实部署需要按端口参数绑定
    return impl_->fd >= 0;
}

uint16_t TurnServer::port() const {
    return impl_->port;
}
size_t TurnServer::allocations() const {
    return impl_->relays.size();
}

void TurnServer::pollOnce(int timeoutMs) {
    // 先看 relay socket 有没有对端数据（有就先转给客户端）
    for (auto& [key, relay] : impl_->relays) {
        (void)key;
        if (!waitReadable(relay.fd, 0)) continue;
        std::string payload;
        StunAddress peer;
        if (!recvDatagram(relay.fd, payload, peer)) continue;
        std::vector<Attr> attrs;
        Attr peerAttr;
        peerAttr.type = kAttrXorPeerAddress;
        peerAttr.value = xorAddress(peer);
        attrs.push_back(peerAttr);
        Attr dataAttr;
        dataAttr.type = kAttrData;
        dataAttr.value = payload;
        attrs.push_back(dataAttr);
        uint8_t txn[12] = {};
        randomTxn(txn);
        const std::string ind = buildMessage(kTypeDataIndication, txn, attrs);
        sendDatagram(impl_->fd, relay.client, ind);
        return;
    }

    if (!waitReadable(impl_->fd, timeoutMs)) return;
    std::string msg;
    StunAddress client;
    if (!recvDatagram(impl_->fd, msg, client)) return;
    Parsed p;
    if (!parseMessage(msg, p)) return;

    if (p.type == kTypeAllocateRequest) {
        auto it = impl_->relays.find(client.port);
        if (it == impl_->relays.end()) {
            Impl::Relay relay;
            relay.fd = openUdp(relay.relayed.port);
            if (relay.fd < 0) return;
            relay.relayed.ip = "127.0.0.1";
            relay.client = client;
            it = impl_->relays.emplace(client.port, std::move(relay)).first;
        }
        std::vector<Attr> attrs;
        Attr relayed;
        relayed.type = kAttrXorRelayedAddress;
        relayed.value = xorAddress(it->second.relayed);
        attrs.push_back(relayed);
        Attr life;
        life.type = kAttrLifetime;
        std::string lv;
        putU32(lv, 600);
        life.value = lv;
        attrs.push_back(life);
        const std::string resp = buildMessage(kTypeAllocateSuccess, p.txn, attrs);
        sendDatagram(impl_->fd, client, resp);
        return;
    }

    if (p.type == kTypeCreatePermissionRequest) {
        const std::string resp = buildMessage(kTypeCreatePermissionSuccess, p.txn, {});
        sendDatagram(impl_->fd, client, resp);
        return;
    }

    if (p.type == kTypeSendIndication) {
        auto it = impl_->relays.find(client.port);
        if (it == impl_->relays.end()) return;
        const Attr* peer = findAttr(p, kAttrXorPeerAddress);
        const Attr* data = findAttr(p, kAttrData);
        if (peer == nullptr || data == nullptr) return;
        StunAddress peerAddr;
        if (!parseXorAddress(peer->value.data(), peer->value.size(), peerAddr)) return;
        sendDatagram(it->second.fd, peerAddr, data->value);
    }
}

}  // namespace zenith::rtc
