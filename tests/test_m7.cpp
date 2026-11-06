// M7 性能批测试：
//   - M7.2 连接缓冲空闲缩容（数据完整性/容量归还）
//   - M7.2a 连接级 Context 池化（keep-alive 复用不串响应头）
//   - M7.3 静态文件零拷贝（完整性/Range/HEAD/回退路径）
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using socket_t = SOCKET;
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using socket_t = int;
#endif

#include "ztest.hpp"

#include "zenith/core/buffer.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/static_files.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ============================ M7.2c 缓冲缩容 ============================ */

ZTEST(M7Buffer, ShrinkKeepsUnreadData) {
    // 构造「容量很大 + 已消费大部分」的形态：readerIndex 远大于前部预留区，
    // 正是此前 resize-before-compact 会搬错数据的区间
    std::string payload(8192, 'a');
    payload[8180] = 'T';
    payload[8191] = 'Z';

    core::Buffer b;
    b.appendString(payload);
    b.consume(8100);
    ZEXPECT_EQ(b.readableBytes(), size_t{92});

    b.shrinkIfBloated(1024); // 8192 > 1024 且 92 < 256 → 触发缩容

    ZEXPECT_EQ(b.readableBytes(), size_t{92});
    const std::string rest = b.readAllAsString();
    ZEXPECT_EQ(rest, payload.substr(8100));
    ZEXPECT_TRUE(rest.back() == 'Z');
    ZEXPECT_TRUE(b.capacity() < 8192 / 2); // 容量确实降下来了
}

ZTEST(M7Buffer, ShrinkReleasesBigCapacity) {
    core::Buffer b;
    b.appendString(std::string(4 * 1024 * 1024, 'x'));
    b.consume(4 * 1024 * 1024 - 16); // 只剩 16 字节

    b.shrinkIfBloated(256 * 1024);
    ZEXPECT_EQ(b.readableBytes(), size_t{16});
    ZEXPECT_EQ(b.capacity(), core::Buffer::kInitialCapacity); // 归还到大缓冲以下
}

ZTEST(M7Buffer, ShrinkNoopWhenIdleOrSmall) {
    core::Buffer small;
    small.appendString("hello");
    const size_t cap = small.capacity();
    small.shrinkIfBloated(256 * 1024);
    ZEXPECT_EQ(small.capacity(), cap); // 未超阈值：不动
    ZEXPECT_EQ(small.readAllAsString(), std::string("hello"));

    // 容量大但仍有大量堆积：不该缩（否则刚写满的缓冲马上又要扩容）
    core::Buffer busy;
    busy.appendString(std::string(2 * 1024 * 1024, 'y'));
    const size_t busyCap = busy.capacity();
    busy.shrinkIfBloated(1024);
    ZEXPECT_EQ(busy.capacity(), busyCap);
    ZEXPECT_EQ(busy.readableBytes(), size_t{2 * 1024 * 1024});
}

/* ============================ HTTP 夹具 ============================ */

namespace {

/// 极简阻塞 socket 客户端（keep-alive 复用测试用；HttpClient 每次新建连接）
class RawConn {
public:
    ~RawConn() { close(); }

    bool open(uint16_t port) {
#if defined(_WIN32)
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (fd_ == static_cast<socket_t>(-1)) return false;
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

    /// 读至少 minBytes（阻塞 + 超时）
    std::string readAtLeast(size_t minBytes, int timeoutMs) {
        std::string out;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
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
            const int sel = ::select(static_cast<int>(fd_ + 1), &rd, nullptr, nullptr, &tv);
            if (sel <= 0) break;
            char buf[65536];
            const int n = ::recv(fd_, buf, sizeof(buf), 0);
            if (n <= 0) break;
            out.append(buf, static_cast<size_t>(n));
        }
        return out;
    }

    void close() {
        if (fd_ == static_cast<socket_t>(-1)) return;
#if defined(_WIN32)
        closesocket(fd_);
#else
        ::close(fd_);
#endif
        fd_ = static_cast<socket_t>(-1);
    }

private:
    socket_t fd_ = static_cast<socket_t>(-1);
};

/// 从原始字节流顺序切出 N 个响应（头 + Content-Length 正文）
bool parseResponses(const std::string& raw, size_t want,
                    std::vector<std::pair<std::string, std::string>>& out) {
    size_t pos = 0;
    for (size_t i = 0; i < want; ++i) {
        const size_t headEnd = raw.find("\r\n\r\n", pos);
        if (headEnd == std::string::npos) return false;
        const std::string head = raw.substr(pos, headEnd - pos);
        // 小写拷贝里找 content-length
        std::string lower = head;
        for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        size_t len = 0;
        const size_t cl = lower.find("content-length:");
        if (cl != std::string::npos) {
            size_t b = cl + 15;
            while (b < lower.size() && (lower[b] == ' ' || lower[b] == '\t')) ++b; // 跳过冒号后的空格
            size_t e = b;
            while (e < lower.size() && lower[e] >= '0' && lower[e] <= '9') ++e;
            if (e > b) len = static_cast<size_t>(std::stoull(lower.substr(b, e - b)));
        }
        const size_t bodyStart = headEnd + 4;
        if (raw.size() < bodyStart + len) return false;
        out.emplace_back(head, raw.substr(bodyStart, len));
        pos = bodyStart + len;
    }
    return true;
}

struct M7Fixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    std::string docRoot = "zm7www.tmp";
    std::string bigBody;   // 2MB+123 可校验内容
    std::string textBody;  // 400KB 文本（可压缩类型 → 缓冲区路径）

    M7Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(docRoot, ec);
        std::filesystem::create_directories(docRoot, ec);

        bigBody.resize(2 * 1024 * 1024 + 123);
        for (size_t i = 0; i < bigBody.size(); ++i) {
            bigBody[i] = static_cast<char>((i * 31u + 7u) & 0xFFu);
        }
        {
            std::ofstream f(docRoot + "/big.png", std::ios::binary);
            f.write(bigBody.data(), static_cast<std::streamsize>(bigBody.size()));
        }
        textBody.assign(400 * 1024, 'T');
        {
            std::ofstream f(docRoot + "/big.txt", std::ios::binary);
            f.write(textBody.data(), static_cast<std::streamsize>(textBody.size()));
        }

        io.start();
        server.router().get("/assets/*path", http::staticHandler(docRoot, "/assets"));
        // 池化隔离用：/hdr 会设一个自定义头，/plain 不该带上
        server.router().get("/hdr", [](http::Context& c) {
            c.setHeader("X-Marker", "one");
            c.text(200, "first");
        });
        server.router().get("/plain", [](http::Context& c) { c.text(200, "second"); });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(150ms);
    }

    ~M7Fixture() {
        loop.stop();
        runner.join();
        io.stop();
        std::error_code ec;
        std::filesystem::remove_all(docRoot, ec);
    }

    std::string url(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(server.port()) + path;
    }
};

} // namespace

/* ============================ M7.3 零拷贝 ============================ */

ZTEST(M7ZeroCopy, BigFileIntegrity) {
    M7Fixture f;
    auto r = http::HttpClient::fetchBlockingUrl(f.url("/assets/big.png"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().body.size(), f.bigBody.size());
    ZEXPECT_TRUE(r.value().body == f.bigBody); // 逐字节一致（含尾部零散字节）
    ZEXPECT_EQ(r.value().headers.get("Content-Type"), std::string("image/png"));
    ZEXPECT_EQ(r.value().headers.get("Content-Length"), std::to_string(f.bigBody.size()));
}

ZTEST(M7ZeroCopy, RangeSlice) {
    M7Fixture f;
    http::ClientRequest rq;
    rq.url = f.url("/assets/big.png");
    rq.headers.set("Range", "bytes=1000-1999");
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 206);
    ZEXPECT_EQ(r.value().body.size(), size_t{1000});
    ZEXPECT_TRUE(r.value().body == f.bigBody.substr(1000, 1000));
    ZEXPECT_EQ(r.value().headers.get("Content-Range"),
               "bytes 1000-1999/" + std::to_string(f.bigBody.size()));
}

ZTEST(M7ZeroCopy, HeadHasLengthButNoBody) {
    M7Fixture f;
    http::ClientRequest rq;
    rq.url = f.url("/assets/big.png");
    rq.method = http::HttpMethod::Head;
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_TRUE(r.value().body.empty());
    // HEAD 必须报真实的正文长度（无 sendfile 分支时也一致）
    ZEXPECT_EQ(r.value().headers.get("Content-Length"), std::to_string(f.bigBody.size()));
}

ZTEST(M7ZeroCopy, TextWithGzipAcceptFallsBackToBuffer) {
    M7Fixture f;
    http::ClientRequest rq;
    rq.url = f.url("/assets/big.txt");
    rq.headers.set("Accept-Encoding", "gzip"); // 可压缩类型 → 不走零拷贝，避免跳过压缩
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().body.size(), f.textBody.size());
    ZEXPECT_TRUE(r.value().body == f.textBody);
}

/* ============================ M7.3 零拷贝：响应顺序 ============================ */

ZTEST(M7ZeroCopy, PipelinedResponseQueuesBehindFileBody) {
    // 场景：客户端把第二条请求与第一条一起发来（HTTP/1.1 pipelining）。
    // 第一条是大文件零拷贝正文，服务器在文件还没发完时就处理了第二条——
    // 第二条的字节绝不能插进文件正文中间，必须等文件发完再发。
    M7Fixture f;
    RawConn c;
    ZEXPECT_TRUE(c.open(f.server.port()));
    const std::string reqs =
        "GET /assets/big.png HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ZEXPECT_TRUE(c.sendAll(reqs));

    const std::string raw = c.readAtLeast(f.bigBody.size() + 512, 15000);
    std::vector<std::pair<std::string, std::string>> rs;
    ZEXPECT_TRUE(parseResponses(raw, 2, rs)); // 两条都完整，说明没有交错
    if (rs.size() == 2) {
        ZEXPECT_EQ(rs[0].first.rfind("HTTP/1.1 200", 0), size_t{0});
        ZEXPECT_TRUE(rs[0].second.size() == f.bigBody.size());
        ZEXPECT_TRUE(rs[0].second == f.bigBody); // 正文一个字节都不能被污染
        ZEXPECT_EQ(rs[1].second, std::string("second"));
    }
}

/* ============================ M7.2a Context 池化隔离 ============================ */

ZTEST(M7ContextPool, KeepAliveReuseDoesNotLeakResponseState) {    M7Fixture f;
    RawConn c;
    ZEXPECT_TRUE(c.open(f.server.port()));
    // 同一个连接上连发两条请求（HTTP/1.1 keep-alive）：第二条不得继承
    // 第一条响应留下的任何状态（池化 Context 若复位不干净就会串头）
    const std::string reqs =
        "GET /hdr HTTP/1.1\r\nHost: x\r\n\r\n"
        "GET /plain HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
    ZEXPECT_TRUE(c.sendAll(reqs));
    const std::string raw = c.readAtLeast(200, 4000);

    std::vector<std::pair<std::string, std::string>> rs;
    ZEXPECT_TRUE(parseResponses(raw, 2, rs));
    ZEXPECT_EQ(rs.size(), size_t{2});
    if (rs.size() == 2) {
        ZEXPECT_TRUE(rs[0].first.find("X-Marker: one") != std::string::npos);
        ZEXPECT_EQ(rs[0].second, std::string("first"));
        // 关键断言：第二条响应没有第一条的残留头
        ZEXPECT_TRUE(rs[1].first.find("X-Marker") == std::string::npos);
        ZEXPECT_EQ(rs[1].second, std::string("second"));
    }
}

ZTEST_MAIN
