// ============================================================================
//  zenith::io —— COBS 帧编码 + CRC32（edge 设备链路的组帧层）
//  COBS（Consistent Overhead Byte Stuffing）：把任意字节流变成无 0x00 的帧，
//  以 0x00 分隔；帧尾附 CRC32（小端）——链路误码/丢字节可检测。
//  帧格式：[COBS(payload+crc32)][0x00]
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::io {

/// 标准 CRC32（IEEE 802.3，反射多项式 0xEDB88320）
ZENITH_API uint32_t crc32(const uint8_t* data, size_t n, uint32_t seed = 0);

inline uint32_t crc32(const std::string& s, uint32_t seed = 0) {
    return crc32(reinterpret_cast<const uint8_t*>(s.data()), s.size(), seed);
}

/// COBS 编码（不含分隔符与 CRC——调用方按需组装）
ZENITH_API std::string cobsEncode(const uint8_t* data, size_t n);
inline std::string cobsEncode(const std::string& s) {
    return cobsEncode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

/// COBS 解码；输入非法（长度/指针错位）返回 nullopt
ZENITH_API std::optional<std::string> cobsDecode(const uint8_t* data, size_t n);
inline std::optional<std::string> cobsDecode(const std::string& s) {
    return cobsDecode(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// ---------------------------------------------------------------------------
// 带 CRC 的完整帧
// ---------------------------------------------------------------------------
namespace frame {

/// 组帧：payload → COBS(payload + crc32(payload)) + 0x00
[[nodiscard]] inline std::string encode(const std::string& payload) {
    std::string body = payload;
    const uint32_t crc = crc32(payload);
    body.append(reinterpret_cast<const char*>(&crc), 4);
    std::string out = cobsEncode(body);
    out.push_back('\0');
    return out;
}

/// 解帧（单帧字节，不含尾 0x00）；CRC 不匹配返回 nullopt
[[nodiscard]] inline std::optional<std::string> decode(const uint8_t* data, size_t n) {
    auto body = cobsDecode(data, n);
    if (!body.has_value() || body->size() < 4) return std::nullopt;
    const size_t payloadLen = body->size() - 4;
    uint32_t expect = 0;
    std::memcpy(&expect, body->data() + payloadLen, 4);
    if (crc32(reinterpret_cast<const uint8_t*>(body->data()), payloadLen) != expect) {
        return std::nullopt;
    }
    return body->substr(0, payloadLen);
}

/// 增量拆帧器：喂入字节流，产出完整帧（去掉分隔符与 CRC）
class ZENITH_API Deframer {
public:
    /// 喂入数据；返回本次解出的帧（可能 0~N 条）
    std::vector<std::string> feed(const uint8_t* data, size_t n);
    std::vector<std::string> feed(const std::string& s) {
        return feed(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }
    /// 缓冲超限保护（恶意/失步流）：超过上限丢弃缓冲并返回 false
    [[nodiscard]] bool healthy() const noexcept { return healthy_; }
    void reset();

    static constexpr size_t kMaxFrameBytes = 1 << 20; // 1MB

private:
    std::string buf_;
    bool healthy_ = true;
};

} // namespace frame

} // namespace zenith::io
