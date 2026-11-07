// ============================================================================
//  STUN（RFC 5389）编解码 + 同步客户端 + host 候选收集
// ============================================================================
#include "zenith/rtc/stun.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <chrono>
#include <cstring>
#include <random>

namespace zenith::rtc {
namespace {

constexpr uint16_t kAttrMappedAddress = 0x0001;
constexpr uint16_t kAttrXorMappedAddress = 0x0020;
constexpr uint16_t kAttrSoftware = 0x8022;
constexpr uint16_t kAttrErrorCode = 0x0009;

void putU16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

uint16_t getU16(const char* p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}

/// 追加一个属性（自动 4 字节对齐填充）
void addAttr(std::string& msg, uint16_t type, std::string_view value) {
    putU16(msg, type);
    putU16(msg, static_cast<uint16_t>(value.size()));
    msg.append(value.data(), value.size());
    while ((msg.size() % 4) != 0)
        msg.push_back('\0');
}

void addAddressAttr(std::string& msg, uint16_t type, const StunAddress& addr, bool xored,
                    const uint8_t txn[12]) {
    std::string v;
    v.push_back('\0');  // reserved
    v.push_back(0x01);  // IPv4

    in_addr in{};
    ::inet_pton(AF_INET, addr.ip.c_str(), &in);
    uint32_t ip = ::ntohl(in.s_addr);
    uint16_t port = addr.port;
    if (xored) {
        ip ^= kStunMagicCookie;
        port ^= static_cast<uint16_t>(kStunMagicCookie >> 16);
    }
    putU16(v, port);
    v.push_back(static_cast<char>((ip >> 24) & 0xFF));
    v.push_back(static_cast<char>((ip >> 16) & 0xFF));
    v.push_back(static_cast<char>((ip >> 8) & 0xFF));
    v.push_back(static_cast<char>(ip & 0xFF));
    addAttr(msg, type, v);
    (void)txn;
}

/// 头 + 事务 ID；返回可继续追加属性的前缀（长度字段最后回填）
std::string beginMessage(uint16_t type, const uint8_t txn[12]) {
    std::string msg;
    putU16(msg, type);
    putU16(msg, 0);  // 长度占位
    msg.push_back(static_cast<char>((kStunMagicCookie >> 24) & 0xFF));
    msg.push_back(static_cast<char>((kStunMagicCookie >> 16) & 0xFF));
    msg.push_back(static_cast<char>((kStunMagicCookie >> 8) & 0xFF));
    msg.push_back(static_cast<char>(kStunMagicCookie & 0xFF));
    msg.append(reinterpret_cast<const char*>(txn), 12);
    return msg;
}

void finishMessage(std::string& msg) {
    const size_t attrLen = msg.size() - 20;
    msg[2] = static_cast<char>((attrLen >> 8) & 0xFF);
    msg[3] = static_cast<char>(attrLen & 0xFF);
}

bool parseAddress(const char* p, size_t len, bool xored, StunAddress& out) {
    if (len < 8) return false;
    const uint8_t family = static_cast<uint8_t>(p[1]);
    if (family != 0x01) return false;  // 只支持 IPv4
    uint16_t port = getU16(p + 2);
    uint32_t ip = (static_cast<uint32_t>(static_cast<uint8_t>(p[4])) << 24) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(p[5])) << 16) |
                  (static_cast<uint32_t>(static_cast<uint8_t>(p[6])) << 8) |
                  static_cast<uint32_t>(static_cast<uint8_t>(p[7]));
    if (xored) {
        ip ^= kStunMagicCookie;
        port ^= static_cast<uint16_t>(kStunMagicCookie >> 16);
    }
    in_addr in{};
    in.s_addr = ::htonl(ip);
    char buf[INET_ADDRSTRLEN] = {};
    if (::inet_ntop(AF_INET, &in, buf, sizeof(buf)) == nullptr) return false;
    out.ip = buf;
    out.port = port;
    return true;
}

}  // namespace

std::string encodeBindingRequest(uint8_t outTxn[12]) {
    std::mt19937 rng{std::random_device{}()};
    for (int i = 0; i < 12; ++i)
        outTxn[i] = static_cast<uint8_t>(rng() & 0xFF);
    std::string msg = beginMessage(kStunTypeBindingRequest, outTxn);
    finishMessage(msg);
    return msg;
}

std::string encodeBindingResponse(const StunAddress& mapped, const uint8_t txn[12]) {
    std::string msg = beginMessage(kStunTypeBindingSuccess, txn);
    addAddressAttr(msg, kAttrXorMappedAddress, mapped, /*xored=*/true, txn);
    addAttr(msg, kAttrSoftware, "zenith/rtc");
    finishMessage(msg);
    return msg;
}

std::string encodeBindingError(int errorCode, const std::string& reason, const uint8_t txn[12]) {
    std::string msg = beginMessage(kStunTypeBindingError, txn);
    std::string v;
    v.push_back('\0');
    v.push_back('\0');
    v.push_back(static_cast<char>((errorCode / 100) & 0x07));
    v.push_back(static_cast<char>(errorCode % 100));
    v += reason;
    addAttr(msg, kAttrErrorCode, v);
    finishMessage(msg);
    return msg;
}

bool decode(std::string_view data, StunMessage& out, std::string* err) {
    auto fail = [err](const char* m) {
        if (err) *err = m;
        return false;
    };
    if (data.size() < 20) return fail("message shorter than header");
    out = StunMessage{};
    out.type = getU16(data.data());
    const uint16_t declared = getU16(data.data() + 2);
    const uint32_t cookie = (static_cast<uint32_t>(static_cast<uint8_t>(data[4])) << 24) |
                            (static_cast<uint32_t>(static_cast<uint8_t>(data[5])) << 16) |
                            (static_cast<uint32_t>(static_cast<uint8_t>(data[6])) << 8) |
                            static_cast<uint32_t>(static_cast<uint8_t>(data[7]));
    if (cookie != kStunMagicCookie) return fail("bad magic cookie");
    std::memcpy(out.transactionId, data.data() + 8, 12);
    if (data.size() < 20u + declared) return fail("truncated message");

    size_t pos = 20;
    const size_t end = 20 + declared;
    while (pos + 4 <= end) {
        const uint16_t at = getU16(data.data() + pos);
        const uint16_t alen = getU16(data.data() + pos + 2);
        pos += 4;
        if (pos + alen > end) return fail("attribute overruns message");
        const char* value = data.data() + pos;
        if (at == kAttrXorMappedAddress) {
            StunAddress a;
            if (parseAddress(value, alen, true, a)) out.xorMapped = a;
        } else if (at == kAttrMappedAddress) {
            StunAddress a;
            if (parseAddress(value, alen, false, a)) out.mapped = a;
        } else if (at == kAttrSoftware) {
            out.software.assign(value, alen);
        } else if (at == kAttrErrorCode) {
            if (alen >= 4) {
                out.errorCode =
                    (static_cast<uint8_t>(value[2]) & 0x07) * 100 + static_cast<uint8_t>(value[3]);
                out.errorReason.assign(value + 4, alen - 4);
            }
        }
        pos += alen;
        while ((pos % 4) != 0)
            ++pos;  // 属性 4 字节对齐
    }
    return true;
}

/* --------------------------- 同步 Binding 客户端 --------------------------- */

bool StunClient::binding(const std::string& host, uint16_t port, StunAddress& mapped, int timeoutMs,
                         std::string* err) {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    auto fail = [err](const char* m) {
        if (err) *err = m;
        return false;
    };

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, host.c_str(), &server.sin_addr) != 1) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (::getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || res == nullptr) {
            return fail("cannot resolve host");
        }
        server.sin_addr = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr;
        ::freeaddrinfo(res);
    }

    const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
    if (fd < 0) return fail("socket() failed");

    uint8_t txn[12] = {};
    const std::string req = encodeBindingRequest(txn);
    if (::sendto(fd, req.data(), static_cast<int>(req.size()), 0,
                 reinterpret_cast<sockaddr*>(&server),
                 sizeof(server)) != static_cast<int>(req.size())) {
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return fail("sendto failed");
    }

    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(fd, &rd);
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int ready = ::select(fd + 1, &rd, nullptr, nullptr, &tv);
    if (ready <= 0) {
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return fail("timeout");
    }

    char buf[1500];
    const int n = ::recv(fd, buf, sizeof(buf), 0);
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
    if (n <= 0) return fail("recv failed");

    StunMessage msg;
    std::string perr;
    if (!decode(std::string_view(buf, static_cast<size_t>(n)), msg, &perr)) {
        return fail(perr.c_str());
    }
    if (msg.type == kStunTypeBindingError) {
        if (err) *err = "stun error " + std::to_string(msg.errorCode) + ": " + msg.errorReason;
        return false;
    }
    if (msg.type != kStunTypeBindingSuccess) return fail("unexpected stun message type");
    if (std::memcmp(msg.transactionId, txn, 12) != 0) return fail("transaction id mismatch");
    if (!msg.xorMapped && !msg.mapped) return fail("no mapped address in response");
    mapped = msg.xorMapped ? *msg.xorMapped : *msg.mapped;
    return true;
}

/* ------------------------------ ICE 候选 ------------------------------ */

std::vector<IceCandidate> gatherHostCandidates() {
    std::vector<IceCandidate> out;
    char hostname[256] = {};
    if (::gethostname(hostname, sizeof(hostname)) != 0) return out;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(hostname, nullptr, &hints, &res) != 0) return out;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        char ip[INET_ADDRSTRLEN] = {};
        auto* sin = reinterpret_cast<sockaddr_in*>(p->ai_addr);
        if (::inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip)) == nullptr) continue;
        bool dup = false;
        for (const auto& c : out) {
            if (c.ip == ip) {
                dup = true;
                break;
            }
        }
        if (!dup) out.push_back(IceCandidate{ip, 0, "host"});
    }
    ::freeaddrinfo(res);
    return out;
}

}  // namespace zenith::rtc
