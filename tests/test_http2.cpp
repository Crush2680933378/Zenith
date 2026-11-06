// HTTP/2 基础层：帧编解码 + HPACK（RFC 9113 / RFC 7541）
#include <string>
#include <vector>

#include "ztest.hpp"

#include "zenith/http/http2.hpp"

using namespace zenith;

namespace {
std::string hexToBytes(const std::string& hex) {
    std::string out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}
}  // namespace

/* -------------------------------- 帧层 -------------------------------- */

ZTEST(H2Frame, EncodeParseRoundtrip) {
    http::h2::Frame f;
    f.type = http::h2::FrameType::Settings;
    f.flags = 0;
    f.streamId = 0;
    f.payload = std::string("\x00\x03\x00\x00\x00\x64", 6);  // MAX_CONCURRENT_STREAMS=100

    const std::string wire = http::h2::encodeFrame(f);
    ZEXPECT_EQ(wire.size(), http::h2::kFrameHeaderLen + 6);

    auto r = http::h2::parseFrame(wire);
    ZEXPECT_TRUE(r.status == http::h2::ParseResult::Status::Ok);
    ZEXPECT_TRUE(r.frame.type == http::h2::FrameType::Settings);
    ZEXPECT_EQ(r.frame.streamId, 0u);
    ZEXPECT_EQ(r.frame.payload, f.payload);
    ZEXPECT_EQ(r.consumed, wire.size());
}

ZTEST(H2Frame, PendingOnPartial) {
    http::h2::Frame f;
    f.type = http::h2::FrameType::Data;
    f.streamId = 1;
    f.payload = "hello";
    const std::string wire = http::h2::encodeFrame(f);

    // 头都没收全
    auto r1 = http::h2::parseFrame(wire.substr(0, 5));
    ZEXPECT_TRUE(r1.status == http::h2::ParseResult::Status::Pending);
    // 头全了但载荷不全
    auto r2 = http::h2::parseFrame(wire.substr(0, wire.size() - 1));
    ZEXPECT_TRUE(r2.status == http::h2::ParseResult::Status::Pending);
    // 全了
    auto r3 = http::h2::parseFrame(wire);
    ZEXPECT_TRUE(r3.status == http::h2::ParseResult::Status::Ok);
}

ZTEST(H2Frame, RejectsOversizedFrame) {
    // 声明长度 20000 > 默认 16384
    std::string wire;
    wire.push_back(static_cast<char>(0x00));
    wire.push_back(static_cast<char>(0x4E));
    wire.push_back(static_cast<char>(0x20));
    wire.push_back(static_cast<char>(http::h2::FrameType::Data));
    wire.push_back(0);
    wire.append(4, '\0');
    auto r = http::h2::parseFrame(wire);
    ZEXPECT_TRUE(r.status == http::h2::ParseResult::Status::Error);
    ZEXPECT_TRUE(r.error == http::h2::ErrorCode::FrameSizeError);
}

ZTEST(H2Frame, StreamIdMaskedTo31Bits) {
    http::h2::Frame f;
    f.type = http::h2::FrameType::Headers;
    f.streamId = 0xFFFFFFFFu;  // 最高位必须被屏蔽
    auto r = http::h2::parseFrame(http::h2::encodeFrame(f));
    ZEXPECT_TRUE(r.status == http::h2::ParseResult::Status::Ok);
    ZEXPECT_EQ(r.frame.streamId, 0x7FFFFFFFu);
}

/* -------------------------------- HPACK -------------------------------- */

ZTEST(H2Hpack, StaticTableSize) {
    ZEXPECT_EQ(http::h2::hpackStaticTableSize(), size_t{61});
    ZEXPECT_EQ(http::h2::hpackStaticEntry(2).first, std::string(":method"));
    ZEXPECT_EQ(http::h2::hpackStaticEntry(2).second, std::string("GET"));
    ZEXPECT_EQ(http::h2::hpackStaticEntry(61).first, std::string("www-authenticate"));
}

// RFC 7541 附录 C.2.2 / C.2.3 的经典请求头块
ZTEST(H2Hpack, DecodesRfcExample) {
    const std::string block = hexToBytes(
        "8286"                                  // :method GET / :scheme http
        "84"                                    // :path /
        "410f7777772e6578616d706c652e636f6d");  // :authority www.example.com
    http::h2::HpackDecoder dec;
    std::vector<std::pair<std::string, std::string>> out;
    std::string err;
    ZEXPECT_TRUE(dec.decode(block, out, &err));
    ZEXPECT_EQ(out.size(), size_t{4});
    ZEXPECT_EQ(out[0].first, std::string(":method"));
    ZEXPECT_EQ(out[0].second, std::string("GET"));
    ZEXPECT_EQ(out[1].second, std::string("http"));
    ZEXPECT_EQ(out[2].first, std::string(":path"));
    ZEXPECT_EQ(out[3].first, std::string(":authority"));
    ZEXPECT_EQ(out[3].second, std::string("www.example.com"));
}

ZTEST(H2Hpack, EncodeDecodeRoundtrip) {
    http::h2::HpackEncoder enc;
    http::h2::HpackDecoder dec;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {":method", "GET"},
        {":scheme", "https"},
        {":path", "/api/users/42"},
        {":authority", "example.com"},
        {"user-agent", "zenith-test/1.0"},
        {"x-custom", "值-中文-也应该能过"},
    };
    const std::string block = enc.encode(headers);
    std::vector<std::pair<std::string, std::string>> out;
    std::string err;
    ZEXPECT_TRUE(dec.decode(block, out, &err));
    ZEXPECT_EQ(out.size(), headers.size());
    for (size_t i = 0; i < headers.size() && i < out.size(); ++i) {
        ZEXPECT_EQ(out[i].first, headers[i].first);
        ZEXPECT_EQ(out[i].second, headers[i].second);
    }
}

ZTEST(H2Hpack, DynamicTableReuseShrinksSecondBlock) {
    http::h2::HpackEncoder enc;
    const std::vector<std::pair<std::string, std::string>> h = {{"x-trace", "abc123"}};
    const std::string first = enc.encode(h);
    const std::string second = enc.encode(h);
    // 第二次应命中动态表 → 只有一个字节的索引字段
    ZEXPECT_TRUE(second.size() < first.size());
    ZEXPECT_EQ(second.size(), size_t{1});
}

ZTEST(H2Hpack, RejectsHuffmanAndGarbage) {
    http::h2::HpackDecoder dec;
    std::vector<std::pair<std::string, std::string>> out;
    std::string err;
    // 0x40（literal, incremental） + 名字"H"=huffman, len=1, 0xff + 值长度 1
    const std::string huff = std::string("\x40\x81\xff\x01x", 4);
    ZEXPECT_FALSE(dec.decode(huff, out, &err));
    ZEXPECT_TRUE(err.find("huffman") != std::string::npos);

    err.clear();
    out.clear();
    // 索引 0 非法
    ZEXPECT_FALSE(dec.decode(std::string("\x80", 1), out, &err));
    ZEXPECT_FALSE(err.empty());
}

ZTEST_MAIN
