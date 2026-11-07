// ============================================================================
//  RTCP 实现（RFC 3550 §6）
// ============================================================================
#include "zenith/rtc/rtcp.hpp"

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

/// 公共头：V=2 | P | RC/count | PT | length(以 32 位字为单位，不含自身)
std::string beginPacket(uint8_t type, uint8_t count) {
    std::string out;
    out.push_back(static_cast<char>((2 << 6) | (count & 0x1F)));
    out.push_back(static_cast<char>(type));
    putU16(out, 0);  // 长度占位
    return out;
}

void finishPacket(std::string& s) {
    const size_t words = s.size() / 4 - 1;
    s[2] = static_cast<char>((words >> 8) & 0xFF);
    s[3] = static_cast<char>(words & 0xFF);
}

void appendReportBlock(std::string& s, const RtcpReportBlock& b) {
    putU32(s, b.ssrc);
    s.push_back(static_cast<char>(b.fractionLost));
    // 24 位有符号 cumulativeLost
    const uint32_t lost = static_cast<uint32_t>(b.cumulativeLost) & 0x00FFFFFFu;
    s.push_back(static_cast<char>((lost >> 16) & 0xFF));
    s.push_back(static_cast<char>((lost >> 8) & 0xFF));
    s.push_back(static_cast<char>(lost & 0xFF));
    putU32(s, b.extendedHighestSeq);
    putU32(s, b.jitter);
    putU32(s, b.lastSr);
    putU32(s, b.delaySinceSr);
}

RtcpReportBlock readReportBlock(const char* p) {
    RtcpReportBlock b;
    b.ssrc = getU32(p);
    b.fractionLost = static_cast<uint8_t>(p[4]);
    uint32_t lost = (static_cast<uint32_t>(static_cast<uint8_t>(p[5])) << 16) |
                    (static_cast<uint32_t>(static_cast<uint8_t>(p[6])) << 8) |
                    static_cast<uint32_t>(static_cast<uint8_t>(p[7]));
    if ((lost & 0x800000u) != 0) lost |= 0xFF000000u;  // 符号扩展
    b.cumulativeLost = static_cast<int32_t>(lost);
    b.extendedHighestSeq = getU32(p + 8);
    b.jitter = getU32(p + 12);
    b.lastSr = getU32(p + 16);
    b.delaySinceSr = getU32(p + 20);
    return b;
}

}  // namespace

bool parseRtcpCompound(std::string_view data, std::vector<RtcpPacket>& out, size_t& consumed,
                       std::string* err) {
    auto fail = [err](const char* m) {
        if (err != nullptr) *err = m;
        return false;
    };
    out.clear();
    consumed = 0;
    size_t pos = 0;
    while (pos + 4 <= data.size()) {
        const uint8_t b0 = static_cast<uint8_t>(data[pos]);
        if ((b0 >> 6) != 2) return fail("rtcp: bad version");
        const uint8_t count = static_cast<uint8_t>(b0 & 0x1F);
        const uint8_t type = static_cast<uint8_t>(data[pos + 1]);
        const uint16_t words = getU16(data.data() + pos + 2);
        const size_t len = (static_cast<size_t>(words) + 1) * 4;
        if (len < 4 || pos + len > data.size()) return fail("rtcp: truncated packet");

        const char* body = data.data() + pos + 4;
        const size_t bodyLen = len - 4;
        RtcpPacket pkt;
        pkt.type = type;
        pkt.count = count;

        if (type == kRtcpSenderReport) {
            const size_t need = 24 + static_cast<size_t>(count) * 24;
            if (bodyLen < need) return fail("rtcp: short SR");
            pkt.sr.ssrc = getU32(body);
            pkt.sr.ntpTimestamp =
                (static_cast<uint64_t>(getU32(body + 4)) << 32) | getU32(body + 8);
            pkt.sr.rtpTimestamp = getU32(body + 12);
            pkt.sr.packetCount = getU32(body + 16);
            pkt.sr.octetCount = getU32(body + 20);
            for (uint8_t i = 0; i < count; ++i) {
                pkt.sr.reports.push_back(readReportBlock(body + 24 + i * 24));
            }
        } else if (type == kRtcpReceiverReport) {
            const size_t need = 4 + static_cast<size_t>(count) * 24;
            if (bodyLen < need) return fail("rtcp: short RR");
            pkt.rr.ssrc = getU32(body);
            for (uint8_t i = 0; i < count; ++i) {
                pkt.rr.reports.push_back(readReportBlock(body + 4 + i * 24));
            }
        } else if (type == kRtcpSdes) {
            if (count == 0) return fail("rtcp: SDES without chunks");
            // 只解第一个 chunk 的 CNAME（WebRTC 必需项）
            if (bodyLen < 8) return fail("rtcp: short SDES");
            const uint32_t ssrc = getU32(body);
            (void)ssrc;
            size_t off = 4;
            // chunk 以 SSRC + item 列表 + 0x00 结束，整块补到 32 位
            while (off + 2 <= bodyLen) {
                const uint8_t itemType = static_cast<uint8_t>(body[off]);
                if (itemType == 0) break;
                const uint8_t itemLen = static_cast<uint8_t>(body[off + 1]);
                if (off + 2 + itemLen > bodyLen) return fail("rtcp: SDES overrun");
                if (itemType == 1) {  // CNAME
                    pkt.cname.assign(body + off + 2, itemLen);
                }
                off += 2 + static_cast<size_t>(itemLen);
            }
        } else if (type == kRtcpBye) {
            const size_t need = static_cast<size_t>(count) * 4;
            if (bodyLen < need) return fail("rtcp: short BYE");
            for (uint8_t i = 0; i < count; ++i) {
                pkt.byeSsrcs.push_back(getU32(body + i * 4));
            }
            if (bodyLen > need) {
                const uint8_t reasonLen = static_cast<uint8_t>(body[need]);
                if (need + 1 + reasonLen <= bodyLen) {
                    pkt.reason.assign(body + need + 1, reasonLen);
                }
            }
        } else {
            return fail("rtcp: unsupported packet type");
        }

        out.push_back(std::move(pkt));
        pos += len;
    }
    if (pos != data.size()) return fail("rtcp: trailing bytes");
    consumed = pos;
    return true;
}

std::string serializeRtcp(const RtcpSenderReportPacket& sr) {
    std::string s = beginPacket(kRtcpSenderReport, static_cast<uint8_t>(sr.reports.size()));
    putU32(s, sr.ssrc);
    putU32(s, static_cast<uint32_t>(sr.ntpTimestamp >> 32));
    putU32(s, static_cast<uint32_t>(sr.ntpTimestamp & 0xFFFFFFFFu));
    putU32(s, sr.rtpTimestamp);
    putU32(s, sr.packetCount);
    putU32(s, sr.octetCount);
    for (const auto& b : sr.reports)
        appendReportBlock(s, b);
    finishPacket(s);
    return s;
}

std::string serializeRtcp(const RtcpReceiverReportPacket& rr) {
    std::string s = beginPacket(kRtcpReceiverReport, static_cast<uint8_t>(rr.reports.size()));
    putU32(s, rr.ssrc);
    for (const auto& b : rr.reports)
        appendReportBlock(s, b);
    finishPacket(s);
    return s;
}

std::string serializeRtcpCompound(uint32_t ssrc, const std::string& cname) {
    // RR（RC=0）
    RtcpReceiverReportPacket rr;
    rr.ssrc = ssrc;
    std::string s = serializeRtcp(rr);

    // SDES：chunk = SSRC + (CNAME, len, value) + 0x00，补到 32 位
    std::string sdes = beginPacket(kRtcpSdes, 1);
    putU32(sdes, ssrc);
    sdes.push_back(static_cast<char>(1));  // CNAME
    sdes.push_back(static_cast<char>(cname.size()));
    sdes += cname;
    sdes.push_back('\0');  // item list 结束
    while ((sdes.size() % 4) != 0)
        sdes.push_back('\0');
    finishPacket(sdes);
    s += sdes;
    return s;
}

}  // namespace zenith::rtc
