// ============================================================================
//  M7.4 响应压缩测试：gzip 往返 + Accept-Encoding 协商 + 端到端
//  zlib 后端缺失时自动跳过（降级为 identity，不视为失败）。
// ============================================================================
#include <cstdio>
#include <string>
#include <thread>

#include "ztest.hpp"

#include "zenith/http/compress.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

bool zlibAvailable() {
    const auto probe = http::gzipCompress("zenith-gzip-probe");
    return probe.has_value() && probe->size() > 2 && probe->data()[0] == '\x1f' &&
           probe->data()[1] == '\x8b'; // gzip magic
}

std::string bigBody(size_t n) {
    std::string s;
    s.reserve(n);
    while (s.size() < n) s += "zenith-streaming-compression|";
    return s;
}

} // namespace

ZTEST(Compress, GzipRoundTrip) {
    if (!zlibAvailable()) {
        std::printf("[ SKIP ] zlib backend not built\n");
        return;
    }
    const std::string original = bigBody(8192);
    auto packed = http::gzipCompress(original, 6);
    ZEXPECT_TRUE(packed.has_value());
    if (!packed.has_value()) return;
    ZEXPECT_TRUE(packed->size() < original.size() / 2); // 高度重复文本应显著变小

    auto back = http::gzipDecompress(*packed);
    ZEXPECT_TRUE(back.has_value());
    if (back.has_value()) ZEXPECT_EQ(*back, original);
}

ZTEST(Compress, NegotiationRules) {
    if (!zlibAvailable()) {
        std::printf("[ SKIP ] zlib backend not built\n");
        return;
    }
    const std::string body = bigBody(4096);

    // 1) 客户端接受 gzip 且体积够大 → 压缩
    {
        http::HttpRequest req;
        req.headers.set("Accept-Encoding", "deflate, gzip;q=1.0");
        http::Context ctx(std::move(req));
        ctx.response().body = body;
        ZEXPECT_TRUE(http::applyGzip(ctx.request(), ctx.response(), 1024));
        ZEXPECT_EQ(ctx.response().headers.get("Content-Encoding"), std::string("gzip"));
        auto back = http::gzipDecompress(ctx.response().body);
        ZEXPECT_TRUE(back.has_value());
        if (back.has_value()) ZEXPECT_EQ(*back, body);
    }
    // 2) 不接受 → 不压缩
    {
        http::HttpRequest req;
        http::Context ctx(std::move(req));
        ctx.response().body = body;
        ZEXPECT_FALSE(http::applyGzip(ctx.request(), ctx.response(), 1024));
        ZEXPECT_FALSE(ctx.response().headers.contains("Content-Encoding"));
    }
    // 3) 明确拒绝 q=0 → 不压缩
    {
        http::HttpRequest req;
        req.headers.set("Accept-Encoding", "gzip;q=0");
        http::Context ctx(std::move(req));
        ctx.response().body = body;
        ZEXPECT_FALSE(http::applyGzip(ctx.request(), ctx.response(), 1024));
    }
    // 4) 体积未达阈值 → 不压缩
    {
        http::HttpRequest req;
        req.headers.set("Accept-Encoding", "gzip");
        http::Context ctx(std::move(req));
        ctx.response().body = "small";
        ZEXPECT_FALSE(http::applyGzip(ctx.request(), ctx.response(), 1024));
    }
    // 5) 204 → 不压缩
    {
        http::HttpRequest req;
        req.headers.set("Accept-Encoding", "gzip");
        http::Context ctx(std::move(req));
        ctx.response().status = 204;
        ctx.response().body = body;
        ZEXPECT_FALSE(http::applyGzip(ctx.request(), ctx.response(), 16));
    }
}

namespace {
struct CompressFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    std::string payload = bigBody(6000);

    CompressFixture() {
        io.start();
        auto& r = server.router();
        r.use(http::gzipMiddleware(512));
        r.get("/big", [this](http::Context& c) {
            c.setHeader("Content-Type", "application/json");
            c.response().body = payload;
        });
        r.get("/tiny", [](http::Context& c) { c.text(200, "tiny"); });

        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }
    ~CompressFixture() {
        loop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] std::string base() const {
        return "http://127.0.0.1:" + std::to_string(server.port());
    }
};
} // namespace

ZTEST(Compress, EndToEndGzipResponse) {
    if (!zlibAvailable()) {
        std::printf("[ SKIP ] zlib backend not built\n");
        return;
    }
    CompressFixture f;

    http::ClientRequest rq;
    rq.url = f.base() + "/big";
    rq.headers.set("Accept-Encoding", "gzip");
    auto res = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;

    ZEXPECT_EQ(res.value().headers.get("Content-Encoding"), std::string("gzip"));
    auto back = http::gzipDecompress(res.value().body);
    ZEXPECT_TRUE(back.has_value());
    if (back.has_value()) ZEXPECT_EQ(*back, f.payload);
}

ZTEST(Compress, SmallResponseStaysIdentity) {
    if (!zlibAvailable()) {
        std::printf("[ SKIP ] zlib backend not built\n");
        return;
    }
    CompressFixture f;
    http::ClientRequest rq;
    rq.url = f.base() + "/tiny";
    rq.headers.set("Accept-Encoding", "gzip");
    auto res = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_FALSE(res.value().headers.contains("Content-Encoding"));
    ZEXPECT_EQ(res.value().body, std::string("tiny"));
}

ZTEST_MAIN
