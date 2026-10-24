// ============================================================================
//  zenith::net::mqtt —— MQTT 3.1.1 客户端（自研协议栈，M13 第四批）
//
//  阻塞式最小实现（与 provider_router 的 simpleHttpRequest 同风格）：
//    connect()   CONNECT/CONNACK（clean session，keepalive 秒）
//    publish()   QoS0/1 PUBLISH（QoS1 等 PUBACK）
//    subscribe() 等 SUBACK
//    pollOnce()  收包分发：PUBLISH → onMessage；PINGREQ 自答 PINGRESP
//    pingreq() / disconnect()
//  Remaining Length 变长编码、UTF-8 字符串、报文类型全部按规范手写。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::net::mqtt {

/// 单条收到的消息（PUBLISH 下行）
struct MqttMessage {
    std::string topic;
    std::string payload;
    bool retain = false;
};

class ZENITH_API MqttClient {
public:
    using OnMessage = std::function<void(const MqttMessage&)>;

    /// 建立 TCP + MQTT CONNECT；失败返回 Err（CONNACK 非零也失败）
    static core::Result<MqttClient> connect(const std::string& host, uint16_t port,
                                            const std::string& clientId,
                                            const std::string& username = {},
                                            const std::string& password = {},
                                            int keepAliveSeconds = 60,
                                            int timeoutMs = 10000);
    MqttClient() = default;
    ~MqttClient();
    MqttClient(MqttClient&& o) noexcept;
    MqttClient& operator=(MqttClient&& o) noexcept;
    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    /// 发布（qos0 即发即忘；qos1 阻塞等 PUBACK）
    [[nodiscard]] core::Result<void> publish(const std::string& topic,
                                             std::string_view payload, int qos = 0,
                                             bool retain = false);
    /// 订阅并等 SUBACK（qos 为申请上限）
    [[nodiscard]] core::Result<void> subscribe(const std::string& topic, int qos = 0);

    /// 收一轮数据（timeoutMs 内）→ 分发 onMessage / 应答 PINGREQ；返回收到的包数
    int pollOnce(int timeoutMs = 200);

    /// 心跳 / 主动断开
    [[nodiscard]] core::Result<void> pingreq();
    void disconnect();

    void onMessage(OnMessage cb) { onMessage_ = std::move(cb); }
    [[nodiscard]] bool connected() const noexcept { return fd_ >= 0; }

private:
    explicit MqttClient(int64_t fd) : fd_(fd) {}
    bool sendAll(const std::string& bytes);
    /// 读 n 字节（阻塞；超时/断开返回 false）
    bool recvAll(char* buf, size_t n);
    /// 读一个完整 MQTT 包；headerByte 出参 = 首字节（type<<4 | flags），
    /// 返回体为剩余长度之后的可变头+载荷
    core::Result<std::string> readPacket(uint8_t& headerByte);

    int64_t fd_ = -1;
    OnMessage onMessage_;
};

} // namespace zenith::net::mqtt
