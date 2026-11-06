// ============================================================================
//  zenith::mqtt —— MQTT 3.1.1（协议级别 4）编码/解码 + 主题匹配
//
//  这一层只做「字节 ↔ 包」，不含会话/订阅树/重传：那些在 mqtt_broker.hpp。
//  支持：CONNECT/CONNACK、PUBLISH（QoS0/1）、PUBACK、SUBSCRIBE/SUBACK、
//        UNSUBSCRIBE/UNSUBACK、PINGREQ/PINGRESP、DISCONNECT
//  不支持（明确拒绝而不是静默忽略）：QoS2 全流程、AUTH（v5）、MQTT v5 属性
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::mqtt {

enum class PacketType : uint8_t {
    Connect = 1,
    Connack = 2,
    Publish = 3,
    Puback = 4,
    Pubrec = 5,
    Pubrel = 6,
    Pubcomp = 7,
    Subscribe = 8,
    Suback = 9,
    Unsubscribe = 10,
    Unsuback = 11,
    Pingreq = 12,
    Pingresp = 13,
    Disconnect = 14,
};

/// CONNECT 的返回码（3.1.1 表 3.1）
enum class ConnectReturn : uint8_t {
    Accepted = 0,
    UnacceptableProtocol = 1,
    IdentifierRejected = 2,
    ServerUnavailable = 3,
    BadUsernameOrPassword = 4,
    NotAuthorized = 5,
};

struct ConnectPacket {
    std::string clientId;
    bool cleanSession = true;
    uint16_t keepAlive = 60;
    std::string username;
    std::string password;
    bool hasUsername = false;
    bool hasPassword = false;
    std::string willTopic;
    std::string willMessage;
    uint8_t willQos = 0;
    bool willRetain = false;
    bool hasWill = false;
};

struct PublishPacket {
    std::string topic;
    std::string payload;
    uint16_t packetId = 0;  // QoS>0 才有
    uint8_t qos = 0;
    bool retain = false;
    bool dup = false;
};

struct SubscribePacket {
    uint16_t packetId = 0;
    std::vector<std::pair<std::string, uint8_t>> filters;  // 主题过滤器 + 请求 QoS
};

struct UnsubscribePacket {
    uint16_t packetId = 0;
    std::vector<std::string> filters;
};

struct Packet {
    PacketType type = PacketType::Connect;
    ConnectPacket connect;
    PublishPacket publish;
    SubscribePacket subscribe;
    UnsubscribePacket unsubscribe;
    uint16_t packetId = 0;       // PUBACK/UNSUBACK/SUBACK
    bool sessionPresent = false;  // CONNACK
    uint8_t returnCode = 0;       // CONNACK
    std::vector<uint8_t> codes;   // SUBACK 每项的授予 QoS（0x80 = 失败）
};

struct DecodeResult {
    enum class Status { Pending, Ok, Error };
    Status status = Status::Pending;
    Packet packet;
    size_t consumed = 0;
    std::string error;
};

/// 解码一个包；数据不足返回 Pending（不消费），协议错误返回 Error
ZENITH_API DecodeResult decode(std::string_view buf, size_t maxPacketSize = 1 << 20);

/* ------------------------------ 编码 ------------------------------ */

ZENITH_API std::string encodeConnack(bool sessionPresent, ConnectReturn code);
ZENITH_API std::string encodePublish(const PublishPacket& p);
ZENITH_API std::string encodePuback(uint16_t packetId);
ZENITH_API std::string encodeSuback(uint16_t packetId, const std::vector<uint8_t>& codes);
ZENITH_API std::string encodeUnsuback(uint16_t packetId);
ZENITH_API std::string encodePingresp();

/* ---------------------------- 主题匹配 ---------------------------- */

/// MQTT 通配符匹配：`+` 匹配单层，`#` 匹配剩余多层（只能是最后一层）
ZENITH_API bool topicMatches(std::string_view filter, std::string_view topic);

/// 主题过滤器是否合法（空、含 null、# 位置不对 → false）
ZENITH_API bool validTopicFilter(std::string_view filter);

/// 发布主题是否合法（不能含通配符）
ZENITH_API bool validTopicName(std::string_view topic);

}  // namespace zenith::mqtt
