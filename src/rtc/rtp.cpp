// ============================================================================
//  RTP（RFC 3550）解析与序列化
// ============================================================================
#include "zenith/rtc/rtp.hpp"

namespace zenith::rtc {
namespace {

uint16_t getU16(const char* p) {
    return static_cast<uint16_t>((static_cast<uint8_t>(p[0]) << 8) | static_cast<uint8_t>(p[1]));
}

uint32_t getU32(const char* p) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(p[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(p[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(p[3]));
}

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

}  // namespace

bool parseRtp(std::string_view data, RtpPacket& out, std::string* err) {
    auto fail = [err](const char* m) {
        if (err != nullptr) *err = m;
        return false;
    };
    if (data.size() < kRtpFixedHeaderSize) return fail("rtp: shorter than fixed header");

    out = RtpPacket{};
    const uint8_t b0 = static_cast<uint8_t>(data[0]);
    out.header.version = static_cast<uint8_t>(b0 >> 6);
    if (out.header.version != 2) return fail("rtp: unsupported version");
    out.header.padding = (b0 & 0x20) != 0;
    out.header.extension = (b0 & 0x10) != 0;
    out.header.csrcCount = static_cast<uint8_t>(b0 & 0x0F);

    const uint8_t b1 = static_cast<uint8_t>(data[1]);
    out.header.marker = (b1 & 0x80) != 0;
    out.header.payloadType = static_cast<uint8_t>(b1 & 0x7F);
    out.header.sequence = getU16(data.data() + 2);
    out.header.timestamp = getU32(data.data() + 4);
    out.header.ssrc = getU32(data.data() + 8);

    size_t pos = kRtpFixedHeaderSize;
    if (data.size() < pos + out.header.csrcCount * 4u) return fail("rtp: truncated csrc");
    for (uint8_t i = 0; i < out.header.csrcCount; ++i) {
        out.header.csrc.push_back(getU32(data.data() + pos));
        pos += 4;
    }

    if (out.header.extension) {
        if (data.size() < pos + 4) return fail("rtp: truncated extension header");
        out.header.extensionProfile.assign(data.data() + pos, 2);
        const uint16_t words = getU16(data.data() + pos + 2);
        pos += 4;
        const size_t extLen = static_cast<size_t>(words) * 4;
        if (data.size() < pos + extLen) return fail("rtp: truncated extension data");
        out.header.extensionData.assign(data.data() + pos, extLen);
        pos += extLen;
    }

    size_t end = data.size();
    if (out.header.padding) {
        // 最后一个字节是 padding 长度（含它自己）
        const uint8_t padLen = static_cast<uint8_t>(data[end - 1]);
        if (padLen == 0 || padLen > end - pos) return fail("rtp: bad padding length");
        end -= padLen;
    }
    if (end < pos) return fail("rtp: negative payload length");
    out.payload.assign(data.data() + pos, end - pos);
    return true;
}

std::string serializeRtp(const RtpPacket& p) {
    std::string out;
    out.reserve(kRtpFixedHeaderSize + p.payload.size() + 8);

    const uint8_t csrcCount = static_cast<uint8_t>(p.header.csrc.size() & 0x0F);
    out.push_back(static_cast<char>((p.header.version << 6) | (p.header.padding ? 0x20 : 0) |
                                    (p.header.extension ? 0x10 : 0) | csrcCount));
    out.push_back(static_cast<char>((p.header.marker ? 0x80 : 0) | (p.header.payloadType & 0x7F)));
    putU16(out, p.header.sequence);
    putU32(out, p.header.timestamp);
    putU32(out, p.header.ssrc);
    for (size_t i = 0; i < csrcCount; ++i)
        putU32(out, p.header.csrc[i]);

    if (p.header.extension) {
        std::string profile = p.header.extensionProfile;
        profile.resize(2, '\0');
        out += profile;
        const size_t words = (p.header.extensionData.size() + 3) / 4;
        putU16(out, static_cast<uint16_t>(words));
        out += p.header.extensionData;
        out.append(words * 4 - p.header.extensionData.size(), '\0');
    }

    out += p.payload;

    if (p.header.padding) {
        // RTP padding：补到 4 字节边界，且**最后一个字节等于 padding 长度（含自己）**
        const size_t padCount = 4 - (out.size() % 4);  // 1..4
        out.append(padCount - 1, '\0');
        out.push_back(static_cast<char>(padCount));
    }
    return out;
}

}  // namespace zenith::rtc
