// ============================================================================
//  HTTP/2 基础层实现：帧编解码 + HPACK（RFC 9113 / RFC 7541）
// ============================================================================
#include "zenith/http/http2.hpp"

#include <cstring>
#include <unordered_map>

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

struct HuffCode {
    uint32_t code;
    uint8_t bits;
};
#include "hpack_huffman.inc"  // constexpr HuffCode kHuffTable[257]（由 RFC 附录 B 生成）

/// RFC 7541 §5.2：按位走 Huffman 树（用一次性建立的哈希表查符号）
bool huffmanDecode(std::string_view in, std::string& out, std::string* err) {
    static const auto* index = [] {
        auto* m = new std::unordered_map<uint64_t, int>();
        for (int s = 0; s < 257; ++s) {
            (*m)[(static_cast<uint64_t>(kHuffTable[s].code) << 6) | kHuffTable[s].bits] = s;
        }
        return m;
    }();

    uint32_t code = 0;
    int bits = 0;
    for (char ch : in) {
        const uint8_t byte = static_cast<uint8_t>(ch);
        for (int i = 7; i >= 0; --i) {
            code = (code << 1) | ((byte >> i) & 1u);
            ++bits;
            if (bits > 30) {
                if (err) *err = "huffman: code too long";
                return false;
            }
            const auto it = index->find((static_cast<uint64_t>(code) << 6) | bits);
            if (it == index->end()) continue;
            if (it->second == 256) {  // EOS 不允许出现在字符串里
                if (err) *err = "huffman: EOS symbol";
                return false;
            }
            out.push_back(static_cast<char>(it->second));
            code = 0;
            bits = 0;
        }
    }
    // 末尾补位：必须 <8 位且全 1
    if (bits > 7) {
        if (err) *err = "huffman: bad padding length";
        return false;
    }
    if (bits > 0 && code != (1u << bits) - 1u) {
        if (err) *err = "huffman: bad padding bits";
        return false;
    }
    return true;
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
    if (pos + len > in.size()) {
        if (err) *err = "string: truncated";
        return false;
    }
    const std::string_view body(in.data() + pos, static_cast<size_t>(len));
    pos += static_cast<size_t>(len);
    if (huffman) return huffmanDecode(body, out, err);
    out.assign(body.data(), body.size());
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

/* --------------------------- h2c 服务端连接 --------------------------- */

H2ServerConnection::H2ServerConnection(Handler h) : handler_(std::move(h)) {}

void H2ServerConnection::sendGoAway(std::string& out, ErrorCode code) {
    Frame f;
    f.type = FrameType::GoAway;
    f.streamId = 0;
    f.payload.push_back(0);
    f.payload.push_back(0);
    f.payload.push_back(0);
    f.payload.push_back(0);  // last-stream-id = 0
    putU16(f.payload, static_cast<uint16_t>(code));
    out += encodeFrame(f);
    closed_ = true;
}

std::string H2ServerConnection::feed(std::string_view data) {
    std::string out;
    if (closed_) return out;
    in_.append(data.data(), data.size());

    if (!prefaceSeen_) {
        const std::string_view want = kClientPreface;
        const size_t n = in_.size() < want.size() ? in_.size() : want.size();
        if (std::string_view(in_).substr(0, n) != want.substr(0, n)) {
            error_ = "bad client preface";
            sendGoAway(out, ErrorCode::ProtocolError);
            return out;
        }
        if (in_.size() < want.size()) return out;  // 还没收全
        in_.erase(0, want.size());
        prefaceSeen_ = true;
        Frame s;  // 服务端 SETTINGS：全默认值
        s.type = FrameType::Settings;
        out += encodeFrame(s);
        settingsSent_ = true;
    }

    while (!in_.empty() && !closed_) {
        const ParseResult r = parseFrame(in_);
        if (r.status == ParseResult::Status::Pending) break;
        if (r.status == ParseResult::Status::Error) {
            error_ = "frame error";
            sendGoAway(out, r.error);
            break;
        }
        in_.erase(0, r.consumed);
        handleFrame(r.frame, out);
    }
    return out;
}

void H2ServerConnection::handleFrame(const Frame& f, std::string& out) {
    switch (f.type) {
        case FrameType::Settings: {
            if ((f.flags & 0x1) != 0) return;  // ACK，无需回
            Frame ack;
            ack.type = FrameType::Settings;
            ack.flags = 0x1;
            out += encodeFrame(ack);
            return;
        }
        case FrameType::Ping: {
            Frame ack;
            ack.type = FrameType::Ping;
            ack.flags = 0x1;
            ack.payload = f.payload;
            out += encodeFrame(ack);
            return;
        }
        case FrameType::Headers: {
            auto& req = streams_[f.streamId];
            // 新流初始化发送窗口（默认 65535；对端 SETTINGS 里调小的情况暂不处理）
            if (streamSendWindow_.find(f.streamId) == streamSendWindow_.end()) {
                streamSendWindow_[f.streamId] = 65535;
            }
            const bool endHeaders = (f.flags & 0x4) != 0;
            const bool endStream = (f.flags & 0x1) != 0;
            std::string block = f.payload;
            // 去掉 PADDED / PRIORITY 前缀（简化：只处理无 pad、无 priority 的常见形态）
            if ((f.flags & 0x8) != 0 && !block.empty()) {  // PADDED
                const size_t pad = static_cast<uint8_t>(block[0]);
                if (block.size() < 1 + pad) {
                    error_ = "bad padded header";
                    sendGoAway(out, ErrorCode::ProtocolError);
                    return;
                }
                block = block.substr(1, block.size() - 1 - pad);
            }
            if ((f.flags & 0x20) != 0) {  // PRIORITY
                if (block.size() < 5) {
                    error_ = "bad priority header";
                    sendGoAway(out, ErrorCode::ProtocolError);
                    return;
                }
                block = block.substr(5);
            }
            if (!endHeaders) {
                pendingHeaderBlock_ = std::move(block);
                pendingHeaderStream_ = f.streamId;
                return;
            }
            std::vector<std::pair<std::string, std::string>> fields;
            std::string err;
            if (!decoder_.decode(block, fields, &err)) {
                error_ = "hpack: " + err;
                sendGoAway(out, ErrorCode::CompressionError);
                return;
            }
            for (auto& [name, value] : fields) {
                if (name == ":method")
                    req.method = value;
                else if (name == ":path")
                    req.path = value;
                else if (name == ":authority")
                    req.authority = value;
                else if (name == ":scheme")
                    req.scheme = value;
                else
                    req.headers.emplace_back(std::move(name), std::move(value));
            }
            if (endStream) dispatch(req, f.streamId, out);
            return;
        }
        case FrameType::Continuation: {
            if (pendingHeaderStream_ != f.streamId) {
                error_ = "unexpected continuation";
                sendGoAway(out, ErrorCode::ProtocolError);
                return;
            }
            pendingHeaderBlock_ += f.payload;
            if ((f.flags & 0x4) == 0) return;
            std::vector<std::pair<std::string, std::string>> fields;
            std::string err;
            if (!decoder_.decode(pendingHeaderBlock_, fields, &err)) {
                error_ = "hpack: " + err;
                sendGoAway(out, ErrorCode::CompressionError);
                return;
            }
            auto& req = streams_[f.streamId];
            for (auto& [name, value] : fields) {
                if (name == ":method")
                    req.method = value;
                else if (name == ":path")
                    req.path = value;
                else if (name == ":authority")
                    req.authority = value;
                else if (name == ":scheme")
                    req.scheme = value;
                else
                    req.headers.emplace_back(std::move(name), std::move(value));
            }
            pendingHeaderBlock_.clear();
            pendingHeaderStream_ = 0;
            if ((f.flags & 0x1) != 0) dispatch(req, f.streamId, out);
            return;
        }
        case FrameType::Data: {
            auto& req = streams_[f.streamId];
            req.body += f.payload;
            // 收了多少补多少：否则大请求体（>64KB）会被对端窗口卡死
            replenish(f.streamId, static_cast<uint32_t>(f.payload.size()), out);
            if ((f.flags & 0x1) != 0) dispatch(req, f.streamId, out);
            return;
        }
        case FrameType::RstStream: {
            streams_.erase(f.streamId);
            return;
        }
        case FrameType::GoAway: {
            closed_ = true;
            return;
        }
        case FrameType::WindowUpdate: {
            if (f.payload.size() != 4) return;
            uint32_t inc = 0;
            for (int i = 0; i < 4; ++i) {
                inc = (inc << 8) | static_cast<uint32_t>(static_cast<uint8_t>(f.payload[i]));
            }
            if (f.streamId == 0) {
                connSendWindow_ += inc;
                // 先收集 id 再 flush：flushPending 会 erase，直接遍历容器会失效
                std::vector<uint32_t> ids;
                ids.reserve(pendingData_.size());
                for (const auto& [sid, buf] : pendingData_) {
                    (void)buf;
                    ids.push_back(sid);
                }
                for (uint32_t sid : ids)
                    flushPending(sid, out);
            } else {
                streamSendWindow_[f.streamId] += inc;
                flushPending(f.streamId, out);
            }
            return;
        }
        default:
            return;  // PRIORITY / PUSH_PROMISE 等暂不处理
    }
}

void H2ServerConnection::dispatch(const H2Request& req, uint32_t streamId, std::string& out) {
    H2Response resp;
    if (handler_) handler_(req, resp);

    std::vector<std::pair<std::string, std::string>> fields;
    fields.emplace_back(":status", std::to_string(resp.status));
    for (const auto& [k, v] : resp.headers)
        fields.emplace_back(k, v);

    std::string block = encoder_.encode(fields);

    Frame h;
    h.type = FrameType::Headers;
    h.streamId = streamId;
    h.flags = 0x4;                          // END_HEADERS
    if (resp.body.empty()) h.flags |= 0x1;  // END_STREAM
    h.payload = std::move(block);
    out += encodeFrame(h);

    // 正文按窗口分帧（大响应会被拆成多个 DATA，窗口不足的部分挂起等 WINDOW_UPDATE）
    if (!resp.body.empty()) sendData(streamId, std::move(resp.body), out);
    streams_.erase(streamId);
}

void H2ServerConnection::sendData(uint32_t streamId, std::string body, std::string& out) {
    if (body.empty()) return;
    pendingData_[streamId] += body;
    flushPending(streamId, out);
}

void H2ServerConnection::flushPending(uint32_t streamId, std::string& out) {
    auto it = pendingData_.find(streamId);
    if (it == pendingData_.end()) return;
    std::string& buf = it->second;
    uint32_t& sw = streamSendWindow_[streamId];
    while (!buf.empty()) {
        uint32_t allowed = static_cast<uint32_t>(buf.size());
        if (allowed > connSendWindow_) allowed = connSendWindow_;
        if (allowed > sw) allowed = sw;
        if (allowed > maxFrameSize_) allowed = maxFrameSize_;
        if (allowed == 0) return;  // 两个窗口都干了：等对端 WINDOW_UPDATE
        Frame d;
        d.type = FrameType::Data;
        d.streamId = streamId;
        d.payload.assign(buf.data(), allowed);
        buf.erase(0, allowed);
        connSendWindow_ -= allowed;
        sw -= allowed;
        if (buf.empty()) d.flags = 0x1;  // END_STREAM
        out += encodeFrame(d);
    }
    pendingData_.erase(it);
}

void H2ServerConnection::replenish(uint32_t streamId, uint32_t bytes, std::string& out) {
    if (bytes == 0) return;
    auto putUpdate = [&out](uint32_t sid, uint32_t inc) {
        Frame w;
        w.type = FrameType::WindowUpdate;
        w.streamId = sid;
        w.payload.push_back(static_cast<char>((inc >> 24) & 0xFF));
        w.payload.push_back(static_cast<char>((inc >> 16) & 0xFF));
        w.payload.push_back(static_cast<char>((inc >> 8) & 0xFF));
        w.payload.push_back(static_cast<char>(inc & 0xFF));
        out += encodeFrame(w);
    };
    putUpdate(streamId, bytes);  // 流级
    putUpdate(0, bytes);         // 连接级
}

}  // namespace zenith::http::h2
