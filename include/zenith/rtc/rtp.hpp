// ============================================================================
//  zenith::rtc —— RTP 包（RFC 3550）解析/序列化
//
//  SRTP 建立在 RTP 之上：先有正确的 RTP 头处理，才谈得上加密与认证。
//  这一层只做「字节 ↔ 结构」，不做加密（见 rtc/dtls.hpp 的加密传输）。
//
//  已做：V=2 校验、CSRC 列表、扩展头、padding 去除、序列化往返
//  未做：RTCP（SR/RR/BYE 等）、RTP 头扩展的具体语义（只透传）
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::rtc {

struct RtpHeader {
    uint8_t version = 2;
    bool padding = false;
    bool extension = false;
    uint8_t csrcCount = 0;
    bool marker = false;
    uint8_t payloadType = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    std::vector<uint32_t> csrc;
    /// 扩展头（当 extension=true）：16-bit profile + 16-bit length(32-bit words) + data
    std::string extensionProfile;  // 2 字节
    std::string extensionData;
};

struct RtpPacket {
    RtpHeader header;
    std::string payload;
};

/// 解析一个 RTP 包；版本不对/长度不足返回 false
ZENITH_API bool parseRtp(std::string_view data, RtpPacket& out, std::string* err = nullptr);

/// 序列化（按 header 里的标记重新拼装；csrc/extension/padding 一致处理）
ZENITH_API std::string serializeRtp(const RtpPacket& p);

/// RTP 包最小长度（12 字节固定头）。
/// 注意不能加 ZENITH_API：constexpr 变量带 dllexport 会报"需要外部链接"。
inline constexpr size_t kRtpFixedHeaderSize = 12;

}  // namespace zenith::rtc
