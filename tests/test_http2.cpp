// HTTP/2 基础层：帧编解码 + HPACK（RFC 9113 / RFC 7541）
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/http/http2.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"

using namespace zenith;
using namespace std::chrono_literals;

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

/* ------------------------- h2c 端到端（真实服务器） ------------------------- */

namespace {
/// 极简阻塞 socket，只为 h2 用例服务（超时读，避免 CI 挂死）
class Raw {
public:
    ~Raw() { close(); }
    bool open(uint16_t port) {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ < 0) return false;
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(port);
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&a), sizeof(a)) == 0;
    }
    bool sendAll(const std::string& s) {
        size_t off = 0;
        while (off < s.size()) {
            const int n = ::send(fd_, s.data() + off, static_cast<int>(s.size() - off), 0);
            if (n <= 0) return false;
            off += static_cast<size_t>(n);
        }
        return true;
    }
    /// 读到「至少解析出 endStream 帧」或超时
    std::string readFor(int timeoutMs, size_t minBytes) {
        std::string out;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (out.size() < minBytes) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  deadline - std::chrono::steady_clock::now())
                                  .count();
            if (left <= 0) break;
            fd_set rd;
            FD_ZERO(&rd);
            FD_SET(fd_, &rd);
            timeval tv{};
            tv.tv_sec = static_cast<long>(left / 1000);
            tv.tv_usec = static_cast<long>((left % 1000) * 1000);
            if (::select(static_cast<int>(fd_ + 1), &rd, nullptr, nullptr, &tv) <= 0) break;
            char buf[65536];
            const int n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }
    void close() {
        if (fd_ < 0) return;
#if defined(_WIN32)
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = -1;
    }

private:
    int fd_ = -1;
};
}  // namespace

ZTEST(H2E2E, GetOverH2c) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.router().get("/hello", [](http::Context& c) { c.text(200, "Hello, Zenith!"); });
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&loop] { loop.loop(); });
    std::this_thread::sleep_for(150ms);

    // 断言失败会抛异常：用 RAII 保证 loop 一定停下、线程一定 join
    struct Cleanup {
        net::EventLoop& loop;
        std::thread& t;
        net::EventLoopThreadPool& io;
        ~Cleanup() {
            loop.stop();
            if (t.joinable()) t.join();
            io.stop();
        }
    } cleanup{loop, t, io};

    Raw raw;
    ZEXPECT_TRUE(raw.open(server.port()));

    // preface + 空 SETTINGS
    std::string req(http::h2::kClientPreface);
    http::h2::Frame settings;
    settings.type = http::h2::FrameType::Settings;
    req += http::h2::encodeFrame(settings);

    // HEADERS(stream 1, END_HEADERS|END_STREAM)
    http::h2::HpackEncoder enc;
    const std::vector<std::pair<std::string, std::string>> headers = {
        {":method", "GET"}, {":scheme", "http"}, {":path", "/hello"}, {":authority", "127.0.0.1"}};
    http::h2::Frame h;
    h.type = http::h2::FrameType::Headers;
    h.streamId = 1;
    h.flags = 0x4 | 0x1;
    h.payload = enc.encode(headers);
    req += http::h2::encodeFrame(h);

    ZEXPECT_TRUE(raw.sendAll(req));
    const std::string resp = raw.readFor(3000, 40);
    ZEXPECT_TRUE(resp.size() > 9);

    // 逐帧解析：至少要看到 HEADERS(:status 200) 和 DATA("Hello, Zenith!")
    bool gotHeaders = false, gotBody = false;
    std::string body;
    size_t pos = 0;
    while (pos < resp.size()) {
        auto r = http::h2::parseFrame(std::string_view(resp).substr(pos));
        if (r.status != http::h2::ParseResult::Status::Ok) break;
        pos += r.consumed;
        if (r.frame.type == http::h2::FrameType::Headers && r.frame.streamId == 1) {
            std::vector<std::pair<std::string, std::string>> fields;
            std::string err;
            ZEXPECT_TRUE(enc.encode({}) == enc.encode({}));  // 编码器可用（防呆）
            http::h2::HpackDecoder dec;
            ZEXPECT_TRUE(dec.decode(r.frame.payload, fields, &err));
            for (const auto& [k, v] : fields) {
                if (k == ":status") gotHeaders = (v == "200");
            }
        } else if (r.frame.type == http::h2::FrameType::Data && r.frame.streamId == 1) {
            body += r.frame.payload;
            if ((r.frame.flags & 0x1) != 0) gotBody = true;
        }
    }
    ZEXPECT_TRUE(gotHeaders);
    ZEXPECT_TRUE(gotBody);
    ZEXPECT_EQ(body, std::string("Hello, Zenith!"));

    raw.close();
}

ZTEST_MAIN
