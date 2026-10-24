// ============================================================================
//  MQTT 3.1.1 客户端实现（M13 第四批）
// ============================================================================
#include "zenith/net/mqtt.hpp"

#include <cstring>
#include <mutex>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace zenith::net::mqtt {
namespace {

constexpr uint8_t kTypeConnect = 1, kTypeConnack = 2, kTypePublish = 3,
                  kTypePuback = 4, kTypeSubscribe = 8, kTypeSuback = 9,
                  kTypePingreq = 12, kTypePingresp = 13, kTypeDisconnect = 14;

void closeSock(int64_t fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    ::closesocket(static_cast<SOCKET>(fd));
#else
    ::close(static_cast<int>(fd));
#endif
}

void sockTimeout(int64_t fd, int recvMs, int sendMs) {
#if defined(_WIN32)
    DWORD r = static_cast<DWORD>(recvMs), s = static_cast<DWORD>(sendMs);
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&r), sizeof(r));
    ::setsockopt(static_cast<SOCKET>(fd), SOL_SOCKET, SO_SNDTIMEO,
                 reinterpret_cast<const char*>(&s), sizeof(s));
#else
    timeval tv{};
    tv.tv_sec = recvMs / 1000;
    tv.tv_usec = (recvMs % 1000) * 1000;
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = sendMs / 1000;
    tv.tv_usec = (sendMs % 1000) * 1000;
    ::setsockopt(static_cast<int>(fd), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

/// MQTT UTF-8 字符串：2 字节大端长度 + 字节
void putStr(std::string& out, const std::string& s) {
    out.push_back(static_cast<char>((s.size() >> 8) & 0xFF));
    out.push_back(static_cast<char>(s.size() & 0xFF));
    out += s;
}

/// Remaining Length 变长编码（每字节低 7 位，高位续）
void putRemain(std::string& out, size_t n) {
    do {
        uint8_t b = static_cast<uint8_t>(n % 128);
        n /= 128;
        if (n > 0) b |= 0x80;
        out.push_back(static_cast<char>(b));
    } while (n > 0);
}

uint16_t readU16(const std::string& s, size_t pos) {
    return static_cast<uint16_t>((static_cast<uint8_t>(s[pos]) << 8) |
                                 static_cast<uint8_t>(s[pos + 1]));
}

} // namespace

core::Result<MqttClient> MqttClient::connect(const std::string& host, uint16_t port,
                                             const std::string& clientId,
                                             const std::string& username,
                                             const std::string& password,
                                             int keepAliveSeconds, int timeoutMs) {
#if defined(_WIN32)
    static std::once_flag wsa;
    std::call_once(wsa, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 ||
        res == nullptr) {
        return core::Result<MqttClient>::Err(core::Error(501, "mqtt: dns failed"));
    }
    const int64_t fd = static_cast<int64_t>(
        ::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (fd < 0) {
        ::freeaddrinfo(res);
        return core::Result<MqttClient>::Err(core::Error(502, "mqtt: socket()"));
    }
    sockTimeout(fd, timeoutMs, timeoutMs);
    if (::connect(static_cast<SOCKET>(fd), res->ai_addr,
                  static_cast<int>(res->ai_addrlen)) != 0) {
        ::freeaddrinfo(res);
        closeSock(fd);
        return core::Result<MqttClient>::Err(core::Error(503, "mqtt: connect failed"));
    }
    ::freeaddrinfo(res);

    // CONNECT 可变头 + 载荷
    std::string vh;
    putStr(vh, "MQTT");   // 协议名（3.1.1）
    vh.push_back(4);      // level 4
    uint8_t flags = 0x02; // clean session
    if (!username.empty()) flags |= 0x80;
    if (!password.empty()) flags |= 0x40;
    vh.push_back(static_cast<char>(flags));
    vh.push_back(static_cast<char>((keepAliveSeconds >> 8) & 0xFF));
    vh.push_back(static_cast<char>(keepAliveSeconds & 0xFF));
    std::string payload;
    putStr(payload, clientId);
    if (!username.empty()) putStr(payload, username);
    if (!password.empty()) putStr(payload, password);

    std::string pkt;
    pkt.push_back(static_cast<char>(kTypeConnect << 4));
    putRemain(pkt, vh.size() + payload.size());
    pkt += vh + payload;

    MqttClient c(fd);
    if (!c.sendAll(pkt)) {
        return core::Result<MqttClient>::Err(core::Error(504, "mqtt: send CONNECT"));
    }
    uint8_t h = 0;
    auto ack = c.readPacket(h);
    if (!ack || (h >> 4) != kTypeConnack ||
        ack.value().size() < 2 || ack.value()[1] != 0) {  // [0]=flags [1]=code
        return core::Result<MqttClient>::Err(core::Error(
            505, "mqtt: CONNACK rejected" + (ack ? " code=" +
                     std::to_string(static_cast<int>(
                         static_cast<uint8_t>(ack.value()[1]))) : std::string())));
    }
    return core::Result<MqttClient>::Ok(std::move(c));
}

MqttClient::~MqttClient() {
    if (fd_ >= 0) {
        disconnect();
        closeSock(fd_);
        fd_ = -1;
    }
}

MqttClient::MqttClient(MqttClient&& o) noexcept : fd_(o.fd_), onMessage_(std::move(o.onMessage_)) {
    o.fd_ = -1;
}

MqttClient& MqttClient::operator=(MqttClient&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) {
            disconnect();
            closeSock(fd_);
        }
        fd_ = o.fd_;
        onMessage_ = std::move(o.onMessage_);
        o.fd_ = -1;
    }
    return *this;
}

bool MqttClient::sendAll(const std::string& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = ::send(static_cast<SOCKET>(fd_), bytes.data() + sent,
                             static_cast<int>(bytes.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool MqttClient::recvAll(char* buf, size_t n) {
    size_t got = 0;
    while (got < n) {
        const int r = ::recv(static_cast<SOCKET>(fd_), buf + got,
                             static_cast<int>(n - got), 0);
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

core::Result<std::string> MqttClient::readPacket(uint8_t& headerByte) {
    char head[2];
    if (!recvAll(head, 1)) {
        return core::Result<std::string>::Err(core::Error(510, "mqtt: eof"));
    }
    headerByte = static_cast<uint8_t>(head[0]);
    size_t remain = 0;
    int shift = 0;
    for (;;) { // 剩余长度变长解码
        if (!recvAll(head, 1)) {
            return core::Result<std::string>::Err(core::Error(510, "mqtt: eof in len"));
        }
        const uint8_t b = static_cast<uint8_t>(head[0]);
        remain |= static_cast<size_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 21) {
            return core::Result<std::string>::Err(core::Error(511, "mqtt: bad remain"));
        }
    }
    std::string body(remain, '\0');
    if (remain > 0 && !recvAll(body.data(), remain)) {
        return core::Result<std::string>::Err(core::Error(510, "mqtt: eof in body"));
    }
    return core::Result<std::string>::Ok(std::move(body));
}

core::Result<void> MqttClient::publish(const std::string& topic, std::string_view payload,
                                       int qos, bool retain) {
    if (fd_ < 0) {
        return core::Result<void>::Err(core::Error(520, "mqtt: not connected"));
    }
    if (topic.empty()) {
        return core::Result<void>::Err(core::Error(521, "mqtt: empty topic"));
    }
    if (qos > 1) qos = 1; // 本实现 QoS 上限 1
    uint8_t flags = static_cast<uint8_t>(qos << 1);
    if (retain) flags |= 1;
    std::string vh;
    putStr(vh, topic);
    if (qos >= 1) { // 包标识
        static uint16_t nextPid = 1;
        const uint16_t pid = nextPid++;
        vh.push_back(static_cast<char>((pid >> 8) & 0xFF));
        vh.push_back(static_cast<char>(pid & 0xFF));
    }
    std::string pkt;
    pkt.push_back(static_cast<char>((kTypePublish << 4) | (flags & 0x0F)));
    putRemain(pkt, vh.size() + payload.size());
    pkt += vh;
    pkt.append(payload.data(), payload.size());
    if (!sendAll(pkt)) {
        return core::Result<void>::Err(core::Error(522, "mqtt: send PUBLISH"));
    }
    if (qos >= 1) {
        uint8_t h = 0;
        auto ack = readPacket(h); // 阻塞等 PUBACK
        if (!ack || (h >> 4) != kTypePuback) {
            return core::Result<void>::Err(core::Error(523, "mqtt: no PUBACK"));
        }
    }
    return core::Result<void>::Ok();
}

core::Result<void> MqttClient::subscribe(const std::string& topic, int qos) {
    if (fd_ < 0) {
        return core::Result<void>::Err(core::Error(520, "mqtt: not connected"));
    }
    static uint16_t nextPid = 100;
    const uint16_t pid = nextPid++;
    std::string vh;
    vh.push_back(static_cast<char>((pid >> 8) & 0xFF));
    vh.push_back(static_cast<char>(pid & 0xFF));
    std::string pl;
    putStr(pl, topic);
    pl.push_back(static_cast<char>(qos & 0x03));
    std::string pkt;
    pkt.push_back(static_cast<char>((kTypeSubscribe << 4) | 0x02)); // 保留位必置
    putRemain(pkt, vh.size() + pl.size());
    pkt += vh + pl;
    if (!sendAll(pkt)) {
        return core::Result<void>::Err(core::Error(524, "mqtt: send SUBSCRIBE"));
    }
    uint8_t h = 0;
    auto ack = readPacket(h);
    if (!ack || (h >> 4) != kTypeSuback || ack.value().size() < 3 ||
        (static_cast<uint8_t>(ack.value().back()) & 0x80) != 0) {
        return core::Result<void>::Err(core::Error(525, "mqtt: SUBACK rejected"));
    }
    return core::Result<void>::Ok();
}

int MqttClient::pollOnce(int timeoutMs) {
    if (fd_ < 0) return -1;
    sockTimeout(fd_, timeoutMs, timeoutMs);
    uint8_t h = 0;
    auto pkt = readPacket(h);
    if (!pkt) return 0;
    const uint8_t type = h >> 4;
    const std::string& b = pkt.value();
    if (type == kTypePublish && b.size() >= 2) {
        MqttMessage m;
        const uint8_t qos = (h >> 1) & 0x03;
        m.retain = (h & 0x01) != 0;
        const uint16_t tlen = readU16(b, 0);
        if (static_cast<size_t>(tlen) + 2 > b.size()) return 1;
        m.topic = b.substr(2, tlen);
        size_t pos = 2 + tlen;
        uint16_t pid = 0;
        if (qos >= 1) { // 包标识（QoS0 无）
            if (pos + 2 > b.size()) return 1;
            pid = readU16(b, pos);
            pos += 2;
        }
        m.payload = b.substr(pos);
        if (onMessage_) onMessage_(m);
        if (qos == 1) { // 回 PUBACK（包标识原样）
            std::string ack;
            ack.push_back(static_cast<char>(kTypePuback << 4));
            ack.push_back('\x02');
            ack.push_back(static_cast<char>((pid >> 8) & 0xFF));
            ack.push_back(static_cast<char>(pid & 0xFF));
            sendAll(ack);
        }
        return 1;
    }
    if (type == kTypePingresp) return 1;
    if (type == kTypeDisconnect) { // 服务器断开
        closeSock(fd_);
        fd_ = -1;
        return -1;
    }
    return 1;
}

core::Result<void> MqttClient::pingreq() {
    if (fd_ < 0) {
        return core::Result<void>::Err(core::Error(520, "mqtt: not connected"));
    }
    std::string pkt;
    pkt.push_back(static_cast<char>(kTypePingreq << 4));
    pkt.push_back('\0');
    if (!sendAll(pkt)) {
        return core::Result<void>::Err(core::Error(526, "mqtt: send PINGREQ"));
    }
    uint8_t h = 0;
    auto ack = readPacket(h);
    if (!ack || (h >> 4) != kTypePingresp) {
        return core::Result<void>::Err(core::Error(527, "mqtt: no PINGRESP"));
    }
    return core::Result<void>::Ok();
}

void MqttClient::disconnect() {
    if (fd_ < 0) return;
    std::string pkt;
    pkt.push_back(static_cast<char>(kTypeDisconnect << 4));
    pkt.push_back('\0');
    sendAll(pkt);
    closeSock(fd_);
    fd_ = -1;
}

} // namespace zenith::net::mqtt
