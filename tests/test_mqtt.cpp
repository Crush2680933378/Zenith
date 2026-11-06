// MQTT 3.1.1 编解码 + 主题匹配 + broker 端到端
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/mqtt/mqtt.hpp"
#include "zenith/mqtt/mqtt_broker.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
/// 拼一个 CONNECT（MQTT/4 + flags + keepalive + clientId）
std::string makeConnect(const std::string& clientId, bool clean = true) {
    std::string body;
    body.push_back(0);
    body.push_back(4);
    body += "MQTT";
    body.push_back(4);                                    // protocol level
    body.push_back(static_cast<char>(clean ? 0x02 : 0));  // connect flags
    body.push_back(0);
    body.push_back(60);  // keepalive
    body.push_back(0);
    body.push_back(static_cast<char>(clientId.size()));
    body += clientId;
    std::string out;
    out.push_back(0x10);  // CONNECT
    out.push_back(static_cast<char>(body.size()));
    out += body;
    return out;
}

/// 极简阻塞客户端：只为端到端用例服务
class RawClient {
public:
    ~RawClient() { close(); }
    bool open(uint16_t port) {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    bool send(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const int n = ::send(fd_, s.data() + off, static_cast<int>(s.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    /// 读一个完整包；超时返回 Pending
    mqtt::DecodeResult readPacket(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            auto r = mqtt::decode(buf_);
            if (r.status == mqtt::DecodeResult::Status::Ok) {
                buf_.erase(0, r.consumed);
                return r;
            }
            if (r.status == mqtt::DecodeResult::Status::Error) return r;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) return r;
            if (!readMore(static_cast<int>(left))) return r;
        }
    }
    void close() {
        if (fd_ < 0) return;
#if defined(_WIN32)
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }

private:
    bool readMore(int timeoutMs) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd_, &rd);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        if (::select(static_cast<int>(fd_ + 1), &rd, nullptr, nullptr, &tv) <= 0) return false;
        char buf[4096];
        const int n = ::recv(fd_, buf, sizeof(buf), 0);
        if (n <= 0) return false;
        buf_.append(buf, static_cast<size_t>(n));
        return true;
    }

    int fd_ = -1;
    std::string buf_;
};

std::string makeSubscribe(uint16_t packetId, const std::string& filter, uint8_t qos) {
    std::string body;
    body.push_back(static_cast<char>(packetId >> 8));
    body.push_back(static_cast<char>(packetId & 0xFF));
    body.push_back(static_cast<char>(filter.size() >> 8));
    body.push_back(static_cast<char>(filter.size() & 0xFF));
    body += filter;
    body.push_back(static_cast<char>(qos));
    std::string out;
    out.push_back(0x82);
    out.push_back(static_cast<char>(body.size()));
    out += body;
    return out;
}
}  // namespace

ZTEST(Mqtt, DecodesConnect) {
    const std::string wire = makeConnect("sensor-1");
    auto r = mqtt::decode(wire);
    ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(r.packet.type == mqtt::PacketType::Connect);
    ZEXPECT_EQ(r.packet.connect.clientId, std::string("sensor-1"));
    ZEXPECT_TRUE(r.packet.connect.cleanSession);
    ZEXPECT_EQ(r.packet.connect.keepAlive, 60);
    ZEXPECT_EQ(r.consumed, wire.size());
}

ZTEST(Mqtt, PendingOnPartialPacket) {
    const std::string wire = makeConnect("abc");
    for (size_t cut : {size_t{1}, wire.size() / 2, wire.size() - 1}) {
        auto r = mqtt::decode(wire.substr(0, cut));
        ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Pending);
    }
}

ZTEST(Mqtt, PublishRoundtripQos1) {
    mqtt::PublishPacket p;
    p.topic = "sensors/room1/temp";
    p.payload = "23.5";
    p.qos = 1;
    p.packetId = 42;
    const std::string wire = mqtt::encodePublish(p);

    auto r = mqtt::decode(wire);
    ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(r.packet.type == mqtt::PacketType::Publish);
    ZEXPECT_EQ(r.packet.publish.topic, p.topic);
    ZEXPECT_EQ(r.packet.publish.payload, p.payload);
    ZEXPECT_EQ(r.packet.publish.packetId, 42);
    ZEXPECT_EQ(static_cast<int>(r.packet.publish.qos), 1);
}

ZTEST(Mqtt, DecodesSubscribeWithTwoFilters) {
    std::string body;
    body.push_back(0);
    body.push_back(7);  // packet id 7
    body.push_back(0);
    body.push_back(3);
    body += "a/+";
    body.push_back(1);  // qos1
    body.push_back(0);
    body.push_back(5);
    body += "b/c/#";
    body.push_back(0);  // qos0
    std::string wire;
    wire.push_back(0x82);
    wire.push_back(static_cast<char>(body.size()));
    wire += body;

    auto r = mqtt::decode(wire);
    ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_EQ(r.packet.subscribe.packetId, 7);
    ZEXPECT_EQ(r.packet.subscribe.filters.size(), size_t{2});
    ZEXPECT_EQ(r.packet.subscribe.filters[0].first, std::string("a/+"));
    ZEXPECT_EQ(static_cast<int>(r.packet.subscribe.filters[0].second), 1);
    ZEXPECT_EQ(r.packet.subscribe.filters[1].first, std::string("b/c/#"));
}

ZTEST(Mqtt, EncodesControlPackets) {
    const std::string connack = mqtt::encodeConnack(false, mqtt::ConnectReturn::Accepted);
    ZEXPECT_EQ(connack.size(), size_t{4});
    ZEXPECT_EQ(static_cast<uint8_t>(connack[0]), 0x20);
    ZEXPECT_EQ(static_cast<uint8_t>(connack[2]), 0);  // session present = false
    ZEXPECT_EQ(static_cast<uint8_t>(connack[3]), 0);  // accepted

    const std::string puback = mqtt::encodePuback(42);
    auto r = mqtt::decode(puback);
    ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_EQ(r.packet.packetId, 42);

    const std::string ping = mqtt::encodePingresp();
    auto p = mqtt::decode(ping);
    ZEXPECT_TRUE(p.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(p.packet.type == mqtt::PacketType::Pingresp);
}

ZTEST(Mqtt, RejectsBadPackets) {
    // 未知类型 0
    ZEXPECT_TRUE(mqtt::decode(std::string("\x00\x00", 2)).status ==
                 mqtt::DecodeResult::Status::Error);
    // CONNECT 协议名不对
    std::string bad = makeConnect("x");
    bad[4] = 'X';
    ZEXPECT_TRUE(mqtt::decode(bad).status == mqtt::DecodeResult::Status::Error);
    // PUBLISH QoS2（不支持）
    mqtt::PublishPacket p;
    p.topic = "t";
    p.qos = 2;
    p.packetId = 1;
    ZEXPECT_TRUE(mqtt::decode(mqtt::encodePublish(p)).status ==
                 mqtt::DecodeResult::Status::Error);
}

ZTEST(Mqtt, TopicMatching) {
    ZEXPECT_TRUE(mqtt::topicMatches("sport/tennis/player1", "sport/tennis/player1"));
    ZEXPECT_FALSE(mqtt::topicMatches("sport/tennis/player1", "sport/tennis/player2"));

    ZEXPECT_TRUE(mqtt::topicMatches("sport/+/player1", "sport/tennis/player1"));
    ZEXPECT_FALSE(mqtt::topicMatches("sport/+/player1", "sport/tennis/x/player1"));

    ZEXPECT_TRUE(mqtt::topicMatches("sport/#", "sport"));
    ZEXPECT_TRUE(mqtt::topicMatches("sport/#", "sport/tennis/player1"));
    ZEXPECT_FALSE(mqtt::topicMatches("sport/#", "sports/tennis"));

    ZEXPECT_TRUE(mqtt::topicMatches("#", "anything/at/all"));
    ZEXPECT_TRUE(mqtt::topicMatches("+/+", "a/b"));
    ZEXPECT_FALSE(mqtt::topicMatches("+/+", "a/b/c"));
}

ZTEST(Mqtt, TopicValidation) {
    ZEXPECT_TRUE(mqtt::validTopicName("a/b/c"));
    ZEXPECT_FALSE(mqtt::validTopicName(""));
    ZEXPECT_FALSE(mqtt::validTopicName("a/+/b"));  // 发布主题不能带通配符
    ZEXPECT_FALSE(mqtt::validTopicName("a/#"));

    ZEXPECT_TRUE(mqtt::validTopicFilter("a/+/b"));
    ZEXPECT_TRUE(mqtt::validTopicFilter("a/#"));
    ZEXPECT_TRUE(mqtt::validTopicFilter("#"));
    ZEXPECT_FALSE(mqtt::validTopicFilter("a/#/b"));  // # 必须在最后
    ZEXPECT_FALSE(mqtt::validTopicFilter("a/b+"));   // + 必须独占一层
    ZEXPECT_FALSE(mqtt::validTopicFilter(""));
}

/* ----------------------------- broker 端到端 ----------------------------- */

namespace {
struct BrokerFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    mqtt::Broker broker{&loop, &io};
    std::thread runner;

    BrokerFixture() {
        io.start();
        (void)broker.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }
    ~BrokerFixture() {
        loop.stop();
        if (runner.joinable()) runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t port() const { return broker.port(); }
};
}  // namespace

ZTEST(MqttBroker, ConnectSubscribePublishFanout) {
    BrokerFixture f;

    RawClient sub;
    ZEXPECT_TRUE(sub.open(f.port()));
    ZEXPECT_TRUE(sub.send(makeConnect("sub-1")));
    auto connack = sub.readPacket(2000);
    ZEXPECT_TRUE(connack.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(connack.packet.type == mqtt::PacketType::Connack);

    ZEXPECT_TRUE(sub.send(makeSubscribe(1, "sensors/#", 1)));
    auto suback = sub.readPacket(2000);
    ZEXPECT_TRUE(suback.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(suback.packet.type == mqtt::PacketType::Suback);
    ZEXPECT_EQ(suback.packet.packetId, 1);
    ZEXPECT_EQ(suback.packet.codes.size(), size_t{1});
    ZEXPECT_EQ(static_cast<int>(suback.packet.codes[0]), 1);  // 授予 QoS1

    RawClient pub;
    ZEXPECT_TRUE(pub.open(f.port()));
    ZEXPECT_TRUE(pub.send(makeConnect("pub-1")));
    ZEXPECT_TRUE(pub.readPacket(2000).status == mqtt::DecodeResult::Status::Ok);

    mqtt::PublishPacket p;
    p.topic = "sensors/room1/temp";
    p.payload = "23.5";
    p.qos = 1;
    p.packetId = 99;
    ZEXPECT_TRUE(pub.send(mqtt::encodePublish(p)));

    // 发布方收到 PUBACK
    auto puback = pub.readPacket(2000);
    ZEXPECT_TRUE(puback.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(puback.packet.type == mqtt::PacketType::Puback);
    ZEXPECT_EQ(puback.packet.packetId, 99);

    // 订阅方收到转发
    auto got = sub.readPacket(2000);
    ZEXPECT_TRUE(got.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(got.packet.type == mqtt::PacketType::Publish);
    ZEXPECT_EQ(got.packet.publish.topic, p.topic);
    ZEXPECT_EQ(got.packet.publish.payload, p.payload);
}

ZTEST(MqttBroker, RetainedMessageDeliveredOnSubscribe) {
    BrokerFixture f;

    RawClient pub;
    ZEXPECT_TRUE(pub.open(f.port()));
    ZEXPECT_TRUE(pub.send(makeConnect("pub-ret")));
    ZEXPECT_TRUE(pub.readPacket(2000).status == mqtt::DecodeResult::Status::Ok);

    mqtt::PublishPacket p;
    p.topic = "config/device1";
    p.payload = "{\"on\":true}";
    p.qos = 0;
    p.retain = true;
    ZEXPECT_TRUE(pub.send(mqtt::encodePublish(p)));
    std::this_thread::sleep_for(100ms);  // 等 broker 处理完

    RawClient sub;
    ZEXPECT_TRUE(sub.open(f.port()));
    ZEXPECT_TRUE(sub.send(makeConnect("sub-ret")));
    ZEXPECT_TRUE(sub.readPacket(2000).status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(sub.send(makeSubscribe(2, "config/#", 0)));

    auto retained = sub.readPacket(2000);
    ZEXPECT_TRUE(retained.status == mqtt::DecodeResult::Status::Ok);
    ZEXPECT_TRUE(retained.packet.type == mqtt::PacketType::Publish);
    ZEXPECT_EQ(retained.packet.publish.topic, p.topic);
    ZEXPECT_EQ(retained.packet.publish.payload, p.payload);
    ZEXPECT_TRUE(retained.packet.publish.retain == false);  // 转发时 retain 清零
}

ZTEST(MqttBroker, PingreqWrongFirstPacketRejected) {
    BrokerFixture f;
    RawClient c;
    ZEXPECT_TRUE(c.open(f.port()));
    // 先发 PINGREQ：没有 CONNECT，broker 必须断开
    const std::string ping(1, static_cast<char>(0xC0));
    ZEXPECT_TRUE(c.send(ping + std::string(1, '\0')));
    auto r = c.readPacket(500);
    ZEXPECT_TRUE(r.status == mqtt::DecodeResult::Status::Pending);  // 直接断开，无回包
}

ZTEST_MAIN
