// ============================================================================
//  HTTP/2 基础层实现：帧编解码 + HPACK（RFC 9113 / RFC 7541）
// ============================================================================
#include "zenith/http/http2.hpp"

#include <cstring>

namespace zenith::http::h2 {
namespace {

void putU16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

// ---- HPACK 整数编码（RFC 7541 §5.1）----
void encodeInt(std::string& out, uint8_t prefixBits, uint32_t value, uint8_t firstByteMask) {
    const uint32_t maxPrefix = (1u << prefixBits) - 1;
    if (value < maxPrefix) {
        out.push_back(static_cast<char>(firstByteMask | value));
        return;
    }
    out.push_back(static_cast<char>(firstByteMask | maxPrefix));
    uint32_t rest = value - maxPrefix;
    while (rest >= 128) {
        out.push_back(static_cast<char>((rest % 128) + 128));
        rest /= 128;
    }
    out.push_back(static_cast<char>(rest));
}

// 返回 false = 数据不足 / 溢出
bool decodeInt(std::string_view in, size_t& pos, uint8_t prefixBits, uint64_t& value) {
    if (pos >= in.size()) return false;
    const uint32_t maxPrefix = (1u << prefixBits) - 1;
    uint64_t v = static_cast<unsigned char>(in[pos]) & maxPrefix;
    ++pos;
    if (v < maxPrefix) {
        value = v;
        return true;
    }
    uint32_t shift = 0;
    for (;;) {
        if (pos >= in.size()) return false;
        const uint8_t b = static_cast<uint8_t>(in[pos++]);
        v += static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) break;
        shift += 7;
        if (shift > 28) return false;  // 恶意长度
    }
    value = v;
    return true;
}

void encodeString(std::string& out, std::string_view s) {
    // H=0（不做 Huffman；解码端遇到 H=1 会明确报压缩错误）
    encodeInt(out, 7, static_cast<uint32_t>(s.size()), 0x00);
    out.append(s.data(), s.size());
}

bool decodeString(std::string_view in, size_t& pos, std::string& out, std::string* err) {
    if (pos >= in.size()) {
        if (err) *err = "string: out of bounds";
        return false;
    }
    const bool huffman = (static_cast<uint8_t>(in[pos]) & 0x80) != 0;
    uint64_t len = 0;
    if (!decodeInt(in, pos, 7, len)) {
        if (err) *err = "string: bad length";
        return false;
    }
    if (huffman) {
        // 还没实现 Huffman 表：宁可明确拒绝，也不要静默解出乱码
        if (err) *err = "huffman-coded string not supported yet";
        return false;
    }
    if (pos + len > in.size()) {
        if (err) *err = "string: truncated";
        return false;
    }
    out.assign(in.data() + pos, static_cast<size_t>(len));
    pos += static_cast<size_t>(len);
    return true;
}

// RFC 7541 附录 A 静态表
struct StaticEntry {
    const char* name;
    const char* value;
};
constexpr StaticEntry kStaticTable[] = {
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
constexpr size_t kStaticCount = sizeof(kStaticTable) / sizeof(kStaticTable[0]);

size_t strlen_(const char* s) {
    size_t n = 0;
    while (s[n] != '\0')
        ++n;
    return n;
}

}  // namespace

size_t hpackStaticTableSize() {
    return kStaticCount;
}

const std::pair<std::string, std::string>& hpackStaticEntry(size_t idx) {
    static thread_local std::pair<std::string, std::string> e;
    const size_t i = idx == 0 ? 0 : idx - 1;
    if (i < kStaticCount) {
        e.first = kStaticTable[i].name;
        e.second = kStaticTable[i].value;
    } else {
        e.first.clear();
        e.second.clear();
    }
    return e;
}

/* ------------------------------ 帧编解码 ------------------------------ */

std::string encodeFrame(const Frame& f) {
    std::string out;
    out.reserve(kFrameHeaderLen + f.payload.size());
    const uint32_t len = static_cast<uint32_t>(f.payload.size());
    out.push_back(static_cast<char>((len >> 16) & 0xFF));
    out.push_back(static_cast<char>((len >> 8) & 0xFF));
    out.push_back(static_cast<char>(len & 0xFF));
    out.push_back(static_cast<char>(f.type));
    out.push_back(static_cast<char>(f.flags));
    out.push_back(static_cast<char>((f.streamId >> 24) & 0x7F));
    out.push_back(static_cast<char>((f.streamId >> 16) & 0xFF));
    out.push_back(static_cast<char>((f.streamId >> 8) & 0xFF));
    out.push_back(static_cast<char>(f.streamId & 0xFF));
    out += f.payload;
    return out;
}

ParseResult parseFrame(std::string_view buf, uint32_t maxFrameSize) {
    ParseResult r;
    if (buf.size() < kFrameHeaderLen) return r;  // Pending
    const uint32_t len = (static_cast<uint32_t>(static_cast<uint8_t>(buf[0])) << 16) |
                         (static_cast<uint32_t>(static_cast<uint8_t>(buf[1])) << 8) |
                         static_cast<uint32_t>(static_cast<uint8_t>(buf[2]));
    if (len > maxFrameSize) {
        r.status = ParseResult::Status::Error;
        r.error = ErrorCode::FrameSizeError;
        return r;
    }
    if (buf.size() < kFrameHeaderLen + len) return r;  // Pending
    r.frame.type = static_cast<FrameType>(static_cast<uint8_t>(buf[3]));
    r.frame.flags = static_cast<uint8_t>(buf[4]);
    r.frame.streamId = (static_cast<uint32_t>(static_cast<uint8_t>(buf[5])) & 0x7F) << 24;
    r.frame.streamId |= static_cast<uint32_t>(static_cast<uint8_t>(buf[6])) << 16;
    r.frame.streamId |= static_cast<uint32_t>(static_cast<uint8_t>(buf[7])) << 8;
    r.frame.streamId |= static_cast<uint32_t>(static_cast<uint8_t>(buf[8]));
    r.frame.payload.assign(buf.data() + kFrameHeaderLen, len);
    r.status = ParseResult::Status::Ok;
    r.consumed = kFrameHeaderLen + len;
    return r;
}

/* -------------------------------- HPACK -------------------------------- */

void HpackEncoder::evictTo(size_t limit) {
    while (dynamicSize_ > limit && !dynamic_.empty()) {
        const auto& e = dynamic_.back();
        dynamicSize_ -= e.first.size() + e.second.size() + 32;
        dynamic_.pop_back();
    }
}

void HpackEncoder::insert(const std::string& name, const std::string& value) {
    dynamic_.insert(dynamic_.begin(), {name, value});
    dynamicSize_ += name.size() + value.size() + 32;
    evictTo(maxDynamicSize_);
}

size_t HpackEncoder::findName(const std::string& name) const {
    for (size_t i = 0; i < kStaticCount; ++i) {
        if (name == kStaticTable[i].name) return i + 1;
    }
    for (size_t i = 0; i < dynamic_.size(); ++i) {
        if (name == dynamic_[i].first) return kStaticCount + i + 1;
    }
    return 0;
}

size_t HpackEncoder::findFull(const std::string& name, const std::string& value) const {
    for (size_t i = 0; i < kStaticCount; ++i) {
        if (name == kStaticTable[i].name && value == kStaticTable[i].value) return i + 1;
    }
    for (size_t i = 0; i < dynamic_.size(); ++i) {
        if (name == dynamic_[i].first && value == dynamic_[i].second) return kStaticCount + i + 1;
    }
    return 0;
}

std::string HpackEncoder::encode(const std::vector<std::pair<std::string, std::string>>& headers) {
    std::string out;
    for (const auto& [name, value] : headers) {
        const size_t full = findFull(name, value);
        if (full != 0) {  // 索引头字段
            encodeInt(out, 7, static_cast<uint32_t>(full), 0x80);
            continue;
        }
        const size_t nameIdx = findName(name);
        if (nameIdx != 0) {  // Literal with Incremental Indexing + 索引名
            encodeInt(out, 6, static_cast<uint32_t>(nameIdx), 0x40);
        } else {
            encodeInt(out, 6, 0, 0x40);
            encodeString(out, name);
        }
        encodeString(out, value);
        insert(name, value);
    }
    return out;
}

void HpackDecoder::evictTo(size_t limit) {
    while (dynamicSize_ > limit && !dynamic_.empty()) {
        const auto& e = dynamic_.back();
        dynamicSize_ -= e.first.size() + e.second.size() + 32;
        dynamic_.pop_back();
    }
}

void HpackDecoder::insert(const std::string& name, const std::string& value) {
    dynamic_.insert(dynamic_.begin(), {name, value});
    dynamicSize_ += name.size() + value.size() + 32;
    evictTo(maxDynamicSize_);
}

namespace {
// 把 1-based 索引解成 (name, value)；staticCount 之后是动态表
bool lookup(size_t idx, size_t staticCount,
            const std::vector<std::pair<std::string, std::string>>& dyn, std::string& name,
            std::string& value) {
    if (idx == 0) return false;
    if (idx <= staticCount) {
        name = kStaticTable[idx - 1].name;
        value = kStaticTable[idx - 1].value;
        return true;
    }
    const size_t d = idx - staticCount - 1;
    if (d >= dyn.size()) return false;
    name = dyn[d].first;
    value = dyn[d].second;
    return true;
}
}  // namespace

bool HpackDecoder::indexedName(std::string_view block, size_t& pos, size_t idx, std::string& name,
                               std::string* err) {
    std::string v;
    if (!lookup(idx, kStaticCount, dynamic_, name, v)) {
        if (err) *err = "invalid header index " + std::to_string(idx);
        return false;
    }
    (void)block;
    (void)pos;
    return true;
}

bool HpackDecoder::literal(std::string_view block, size_t& pos, std::string& name,
                           std::string& value, std::string* err) {
    (void)block;
    (void)pos;
    (void)name;
    (void)value;
    (void)err;
    return true;
}

bool HpackDecoder::decode(std::string_view block,
                          std::vector<std::pair<std::string, std::string>>& out, std::string* err) {
    size_t pos = 0;
    while (pos < block.size()) {
        const uint8_t b = static_cast<uint8_t>(block[pos]);
        if ((b & 0x80) != 0) {  // Indexed Header Field
            uint64_t idx = 0;
            if (!decodeInt(block, pos, 7, idx)) {
                if (err) *err = "indexed: bad int";
                return false;
            }
            std::string n, v;
            if (!lookup(static_cast<size_t>(idx), kStaticCount, dynamic_, n, v)) {
                if (err) *err = "indexed: unknown index " + std::to_string(idx);
                return false;
            }
            out.emplace_back(std::move(n), std::move(v));
            continue;
        }
        if ((b & 0xC0) == 0x40) {  // Literal with Incremental Indexing
            uint64_t nameIdx = 0;
            if (!decodeInt(block, pos, 6, nameIdx)) {
                if (err) *err = "literal: bad name index";
                return false;
            }
            std::string n, v;
            if (nameIdx == 0) {
                if (!decodeString(block, pos, n, err)) return false;
            } else if (!lookup(static_cast<size_t>(nameIdx), kStaticCount, dynamic_, n, v)) {
                if (err) *err = "literal: unknown name index " + std::to_string(nameIdx);
                return false;
            }
            if (!decodeString(block, pos, v, err)) return false;
            insert(n, v);
            out.emplace_back(std::move(n), std::move(v));
            continue;
        }
        if ((b & 0xF0) == 0x00 || (b & 0xF0) == 0x10) {  // Without / Never Indexed
            const uint8_t prefix = 4;
            uint64_t nameIdx = 0;
            if (!decodeInt(block, pos, prefix, nameIdx)) {
                if (err) *err = "literal(no-index): bad name index";
                return false;
            }
            std::string n, v;
            if (nameIdx == 0) {
                if (!decodeString(block, pos, n, err)) return false;
            } else if (!lookup(static_cast<size_t>(nameIdx), kStaticCount, dynamic_, n, v)) {
                if (err) *err = "literal(no-index): unknown name index";
                return false;
            }
            if (!decodeString(block, pos, v, err)) return false;
            out.emplace_back(std::move(n), std::move(v));
            continue;
        }
        if ((b & 0xE0) == 0x20) {  // Dynamic Table Size Update
            uint64_t sz = 0;
            if (!decodeInt(block, pos, 5, sz)) {
                if (err) *err = "table size update: bad int";
                return false;
            }
            maxDynamicSize_ = static_cast<size_t>(sz);
            evictTo(maxDynamicSize_);
            continue;
        }
        if (err) *err = "unknown header field representation";
        return false;
    }
    return true;
}

}  // namespace zenith::http::h2
