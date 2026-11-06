// ============================================================================
//  MQTT 3.1.1 编解码 + 主题匹配
// ============================================================================
#include "zenith/mqtt/mqtt.hpp"

namespace zenith::mqtt {
namespace {

struct Reader {
    std::string_view b;
    size_t pos = 0;

    bool need(size_t n) const { return pos + n <= b.size(); }
    bool u8(uint8_t& v) {
        if (!need(1)) return false;
        v = static_cast<uint8_t>(b[pos++]);
        return true;
    }
    bool u16(uint16_t& v) {
        if (!need(2)) return false;
        v = static_cast<uint16_t>((static_cast<uint8_t>(b[pos]) << 8) |
                                  static_cast<uint8_t>(b[pos + 1]));
        pos += 2;
        return true;
    }
    bool str(std::string& s) {
        uint16_t n = 0;
        if (!u16(n)) return false;
        if (!need(n)) return false;
        s.assign(b.data() + pos, n);
        pos += n;
        return true;
    }
    std::string_view rest() const { return b.substr(pos); }
};

void putU16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
    out.push_back(static_cast<char>(v & 0xFF));
}

void putStr(std::string& out, std::string_view s) {
    putU16(out, static_cast<uint16_t>(s.size()));
    out.append(s.data(), s.size());
}

/// 变长剩余长度（每字节 7 位，最多 4 字节）
void putRemainingLength(std::string& out, size_t len) {
    do {
        uint8_t d = len % 128;
        len /= 128;
        if (len > 0) d |= 0x80;
        out.push_back(static_cast<char>(d));
    } while (len > 0);
}

DecodeResult err(std::string msg) {
    DecodeResult r;
    r.status = DecodeResult::Status::Error;
    r.error = std::move(msg);
    return r;
}

}  // namespace

DecodeResult decode(std::string_view buf, size_t maxPacketSize) {
    DecodeResult r;
    if (buf.size() < 2) return r;  // Pending

    const uint8_t first = static_cast<uint8_t>(buf[0]);
    const uint8_t typeNibble = static_cast<uint8_t>(first >> 4);
    const uint8_t flags = static_cast<uint8_t>(first & 0x0F);
    if (typeNibble == 0 || typeNibble > 14) return err("invalid packet type");

    size_t pos = 1;
    size_t len = 0;
    size_t mult = 1;
    for (int i = 0; i < 4; ++i) {
        if (pos >= buf.size()) return r;  // Pending
        const uint8_t d = static_cast<uint8_t>(buf[pos++]);
        len += static_cast<size_t>(d & 0x7F) * mult;
        mult *= 128;
        if ((d & 0x80) == 0) break;
        if (i == 3) return err("remaining length too long");
    }
    if (len > maxPacketSize) return err("packet too large");
    if (buf.size() < pos + len) return r;  // Pending

    const std::string_view body = buf.substr(pos, len);
    Reader rd{body, 0};
    Packet p;
    p.type = static_cast<PacketType>(typeNibble);

    switch (p.type) {
        case PacketType::Connect: {
            std::string proto;
            uint8_t level = 0;
            if (!rd.str(proto) || !rd.u8(level)) return err("connect: truncated");
            if (proto != "MQTT") return err("connect: bad protocol name");
            if (level != 4)
                return err("connect: unsupported protocol level " + std::to_string(level));
            uint8_t cflags = 0;
            if (!rd.u8(cflags) || !rd.u16(p.connect.keepAlive)) return err("connect: truncated");
            if ((cflags & 0x01) != 0) return err("connect: reserved flag set");
            p.connect.cleanSession = (cflags & 0x02) != 0;
            p.connect.hasWill = (cflags & 0x04) != 0;
            p.connect.willQos = static_cast<uint8_t>((cflags >> 3) & 0x03);
            p.connect.willRetain = (cflags & 0x20) != 0;
            p.connect.hasUsername = (cflags & 0x80) != 0;
            p.connect.hasPassword = (cflags & 0x40) != 0;
            if (p.connect.willQos > 1) return err("connect: will qos 2 not supported");
            if (!rd.str(p.connect.clientId)) return err("connect: bad client id");
            if (p.connect.hasWill) {
                if (!rd.str(p.connect.willTopic) || !rd.str(p.connect.willMessage)) {
                    return err("connect: bad will");
                }
            }
            if (p.connect.hasUsername && !rd.str(p.connect.username)) {
                return err("connect: bad username");
            }
            if (p.connect.hasPassword && !rd.str(p.connect.password)) {
                return err("connect: bad password");
            }
            break;
        }
        case PacketType::Publish: {
            p.publish.qos = static_cast<uint8_t>((flags >> 1) & 0x03);
            p.publish.retain = (flags & 0x01) != 0;
            p.publish.dup = (flags & 0x08) != 0;
            if (p.publish.qos > 1) return err("publish: qos 2 not supported");
            if (!rd.str(p.publish.topic)) return err("publish: bad topic");
            if (p.publish.qos > 0 && !rd.u16(p.publish.packetId)) {
                return err("publish: missing packet id");
            }
            p.publish.payload = std::string(rd.rest());
            break;
        }
        case PacketType::Subscribe: {
            if (!rd.u16(p.subscribe.packetId)) return err("subscribe: truncated");
            while (rd.pos < body.size()) {
                std::string filter;
                uint8_t qos = 0;
                if (!rd.str(filter) || !rd.u8(qos)) return err("subscribe: bad filter");
                if (qos > 1) return err("subscribe: qos 2 not supported");
                p.subscribe.filters.emplace_back(std::move(filter), qos);
            }
            if (p.subscribe.filters.empty()) return err("subscribe: no filters");
            break;
        }
        case PacketType::Unsubscribe: {
            if (!rd.u16(p.unsubscribe.packetId)) return err("unsubscribe: truncated");
            while (rd.pos < body.size()) {
                std::string filter;
                if (!rd.str(filter)) return err("unsubscribe: bad filter");
                p.unsubscribe.filters.push_back(std::move(filter));
            }
            if (p.unsubscribe.filters.empty()) return err("unsubscribe: no filters");
            break;
        }
        case PacketType::Connack: {
            uint8_t ackFlags = 0;
            if (!rd.u8(ackFlags) || !rd.u8(p.returnCode)) return err("connack: truncated");
            p.sessionPresent = (ackFlags & 0x01) != 0;
            break;
        }
        case PacketType::Suback: {
            if (!rd.u16(p.packetId)) return err("suback: truncated");
            while (rd.pos < body.size()) {
                uint8_t code = 0;
                if (!rd.u8(code)) return err("suback: bad code");
                p.codes.push_back(code);
            }
            break;
        }
        case PacketType::Puback:
        case PacketType::Unsuback: {
            if (!rd.u16(p.packetId)) return err("ack: truncated");
            break;
        }
        case PacketType::Pingreq:
        case PacketType::Pingresp:
        case PacketType::Disconnect: {
            if (!body.empty()) return err("control packet with payload");
            break;
        }
        default:
            return err("unsupported packet type");
    }

    r.status = DecodeResult::Status::Ok;
    r.packet = std::move(p);
    r.consumed = pos + len;
    return r;
}

/* ------------------------------ 编码 ------------------------------ */

std::string encodeConnack(bool sessionPresent, ConnectReturn code) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(PacketType::Connack) << 4));
    putRemainingLength(out, 2);
    out.push_back(static_cast<char>(sessionPresent ? 1 : 0));
    out.push_back(static_cast<char>(code));
    return out;
}

std::string encodePublish(const PublishPacket& p) {
    std::string body;
    putStr(body, p.topic);
    if (p.qos > 0) putU16(body, p.packetId);
    body += p.payload;

    std::string out;
    uint8_t flags = 0;
    if (p.dup) flags |= 0x08;
    flags |= static_cast<uint8_t>((p.qos & 0x03) << 1);
    if (p.retain) flags |= 0x01;
    out.push_back(static_cast<char>(static_cast<uint8_t>(PacketType::Publish) << 4 | flags));
    putRemainingLength(out, body.size());
    out += body;
    return out;
}

std::string encodePuback(uint16_t packetId) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(PacketType::Puback) << 4));
    putRemainingLength(out, 2);
    putU16(out, packetId);
    return out;
}

std::string encodeSuback(uint16_t packetId, const std::vector<uint8_t>& codes) {
    std::string body;
    putU16(body, packetId);
    for (uint8_t c : codes)
        body.push_back(static_cast<char>(c));
    std::string out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(PacketType::Suback) << 4));
    putRemainingLength(out, body.size());
    out += body;
    return out;
}

std::string encodeUnsuback(uint16_t packetId) {
    std::string out;
    out.push_back(static_cast<char>(static_cast<uint8_t>(PacketType::Unsuback) << 4));
    putRemainingLength(out, 2);
    putU16(out, packetId);
    return out;
}

std::string encodePingresp() {
    return std::string(1, static_cast<char>(static_cast<uint8_t>(PacketType::Pingresp) << 4)) +
           std::string(1, '\0');
}

/* ---------------------------- 主题匹配 ---------------------------- */

bool validTopicName(std::string_view topic) {
    if (topic.empty()) return false;
    for (char c : topic) {
        if (c == '+' || c == '#' || c == '\0') return false;
    }
    return true;
}

bool validTopicFilter(std::string_view filter) {
    if (filter.empty()) return false;
    size_t i = 0;
    while (i < filter.size()) {
        const size_t end = filter.find('/', i);
        const std::string_view level =
            filter.substr(i, end == std::string_view::npos ? std::string_view::npos : end - i);
        for (char c : level) {
            if (c == '\0') return false;
            if (c == '+') {
                if (level.size() != 1) return false;  // "a+" 非法
            } else if (c == '#') {
                if (level != "#") return false;                   // "a#" 非法
                if (end != std::string_view::npos) return false;  // '#' 必须是最后一层
            }
        }
        if (end == std::string_view::npos) break;
        i = end + 1;
    }
    return true;
}

bool topicMatches(std::string_view filter, std::string_view topic) {
    size_t fi = 0;
    size_t ti = 0;
    for (;;) {
        const size_t fe = filter.find('/', fi);
        const std::string_view flevel =
            filter.substr(fi, fe == std::string_view::npos ? std::string_view::npos : fe - fi);

        // '#' 匹配剩余所有层，**包括零层**（"sport/#" 要能匹配 "sport"）
        if (flevel == "#") return true;

        const size_t te = topic.find('/', ti);
        const std::string_view tlevel =
            topic.substr(ti, te == std::string_view::npos ? std::string_view::npos : te - ti);

        if (flevel != "+" && flevel != tlevel) return false;

        const bool fEnd = fe == std::string_view::npos;
        const bool tEnd = te == std::string_view::npos;
        if (tEnd) {
            if (fEnd) return true;
            // 主题用完但过滤器还有：只有剩下的是 "#" 才算匹配
            return filter.substr(fe + 1) == "#";
        }
        if (fEnd) return false;  // 过滤器用完、主题还有层 → 不匹配
        fi = fe + 1;
        ti = te + 1;
    }
}

}  // namespace zenith::mqtt
