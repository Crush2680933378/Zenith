// ============================================================================
//  zenith::rtc —— RTCP（RFC 3550 §6）控制包解析/序列化
//
//  RTP 的数据面在 rtp.hpp；这里是控制面：SR / RR / SDES / BYE。
//  已做：公共头校验、SR 的发送者信息与报告块、RR 报告块、SDES（CNAME）、BYE、复合包遍历
//  未做：APP、XR（扩展报告）、加密（SRTCP 与 SRTP 一起因缺官方向量搁置）
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::rtc {

constexpr uint8_t kRtcpSenderReport = 200;
constexpr uint8_t kRtcpReceiverReport = 201;
constexpr uint8_t kRtcpSdes = 202;
constexpr uint8_t kRtcpBye = 203;

/// 一个接收报告块
struct RtcpReportBlock {
    uint32_t ssrc = 0;
    uint8_t fractionLost = 0;
    int32_t cumulativeLost = 0;  // 24 位有符号
    uint32_t extendedHighestSeq = 0;
    uint32_t jitter = 0;
    uint32_t lastSr = 0;        // LSR
    uint32_t delaySinceSr = 0;  // DLSR
};

/// 发送者报告（SR）
struct RtcpSenderReportPacket {
    uint32_t ssrc = 0;
    uint64_t ntpTimestamp = 0;  // 64 位 NTP
    uint32_t rtpTimestamp = 0;
    uint32_t packetCount = 0;
    uint32_t octetCount = 0;
    std::vector<RtcpReportBlock> reports;
};

/// 接收者报告（RR）
struct RtcpReceiverReportPacket {
    uint32_t ssrc = 0;
    std::vector<RtcpReportBlock> reports;
};

/// 解析结果：一个 RTCP 复合包里的一条
struct RtcpPacket {
    uint8_t type = 0;   // 200..203
    uint8_t count = 0;  // RC / SC
    RtcpSenderReportPacket sr;
    RtcpReceiverReportPacket rr;
    std::vector<uint32_t> byeSsrcs;
    std::string reason;  // BYE reason
    /// SDES：只取第一项 CNAME（WebRTC 必需）
    std::string cname;
};

/// 解析**一个**复合包（可能含多条），返回消费的字节数；失败返回 false
ZENITH_API bool parseRtcpCompound(std::string_view data, std::vector<RtcpPacket>& out,
                                  size_t& consumed, std::string* err = nullptr);

/// 序列化：单个 SR / RR / BYE
ZENITH_API std::string serializeRtcp(const RtcpSenderReportPacket& sr);
ZENITH_API std::string serializeRtcp(const RtcpReceiverReportPacket& rr);
/// 复合包：RR（可选） + SDES(CNAME) —— WebRTC 的标准最小复合包
ZENITH_API std::string serializeRtcpCompound(uint32_t ssrc, const std::string& cname);

}  // namespace zenith::rtc
