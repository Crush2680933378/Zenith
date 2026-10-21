// ============================================================================
//  M9.1 流式响应测试：SSE / chunked 真实回环
//  HttpServer（本仓）←→ HttpClient（本仓解析器解分块）——服务端写端与
//  客户端解析器互为对照组，任一侧分块编码有偏差都会立刻暴露。
// ============================================================================
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>

#include "ztest.hpp"

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

struct StreamFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;

    /// /sse-thread 把写端交给测试主体，用来验证跨线程推送
    std::mutex mtx;
    http::SseStream::Ptr handed;
    std::atomic<bool> handlerOk{true};

    StreamFixture() {
        io.start();
        auto& r = server.router();

        // 同步推送：retry + 三类事件（普通/带 event+id/多行 data）+ 心跳注释
        r.get("/sse", [this](http::Context& c) {
            auto s = c.sse(200, {{"X-Trace", "m9"}});
            if (s == nullptr) {
                handlerOk = false;
                return;
            }
            s->retry(1500);
            s->send("hello");
            s->send("tick", "tick", "1");
            s->send("line1\nline2");
            s->comment("hb");
            s->close();
            if (!s->closed()) handlerOk = false;
        });

        // 跨线程推送：handler 只建流，事件由外部线程续发
        r.get("/sse-thread", [this](http::Context& c) {
            auto s = c.sse();
            if (s == nullptr) {
                handlerOk = false;
                return;
            }
            std::lock_guard<std::mutex> lk(mtx);
            handed = s;
        });

        // 通用分块流
        r.get("/chunked", [this](http::Context& c) {
            auto w = c.chunkedStream("text/plain; charset=utf-8");
            if (w == nullptr) {
                handlerOk = false;
                return;
            }
            w->write("AAA");
            w->write("BBB");
            w->write("CCC");
            w->close();
            if (!w->closed()) handlerOk = false;
        });

        // 只置 chunked 不开流（handler 误用）：服务器须补终止块，客户端不得挂起
        r.get("/chunked-flag-only", [](http::Context& c) {
            c.response().chunked = true;
            c.setHeader("Content-Type", "text/plain");
        });

        // 非流式对照：确认常规响应路径未被改动
        r.get("/plain", [](http::Context& c) { c.text(200, "plain-body"); });

        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms); // 监听生效
    }

    ~StreamFixture() {
        loop.stop();
        runner.join();
        io.stop();
    }

    [[nodiscard]] std::string base() const {
        return "http://127.0.0.1:" + std::to_string(server.port());
    }

    core::Result<http::HttpResponse> fetch(const std::string& path) const {
        return http::HttpClient::fetchBlockingUrl(base() + path);
    }
};

} // namespace

ZTEST(Stream, SseEventsAndFraming) {
    StreamFixture f;
    auto res = f.fetch("/sse");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;

    const auto& resp = res.value();
    ZEXPECT_EQ(resp.status, 200);
    ZEXPECT_TRUE(resp.headers.get("Content-Type").find("text/event-stream") != std::string::npos);
    ZEXPECT_TRUE(resp.headers.get("Content-Type").find("text/event-stream") == 0);
    ZEXPECT_EQ(resp.headers.get("X-Trace"), std::string("m9"));
    ZEXPECT_EQ(resp.headers.get("Transfer-Encoding"), std::string("chunked"));
    ZEXPECT_FALSE(resp.headers.contains("Content-Length"));
    // 事件帧逐字节核对（含多行 data 与心跳注释）
    const std::string want =
        "retry: 1500\n\n"
        "data: hello\n\n"
        "event: tick\n"
        "id: 1\n"
        "data: tick\n\n"
        "data: line1\n"
        "data: line2\n\n"
        ": hb\n\n";
    ZEXPECT_EQ(resp.body, want);
    ZEXPECT_TRUE(f.handlerOk.load());
}

ZTEST(Stream, SseCrossThreadPush) {
    StreamFixture f;

    core::Result<http::HttpResponse> res = core::Result<http::HttpResponse>::Err(
        core::Error(1, "not run"));
    std::thread client([&] { res = f.fetch("/sse-thread"); });

    // 等 handler 把写端交出来（最多 2s）
    http::SseStream::Ptr s;
    for (int i = 0; i < 200 && s == nullptr; ++i) {
        {
            std::lock_guard<std::mutex> lk(f.mtx);
            s = f.handed;
        }
        if (s == nullptr) std::this_thread::sleep_for(10ms);
    }
    ZEXPECT_TRUE(s != nullptr);
    if (s != nullptr) {
        s->send("from-test-1");
        s->send("from-test-2");
        s->close();
    }
    client.join();

    ZEXPECT_TRUE(res.isOk());
    if (res.isOk()) {
        ZEXPECT_EQ(res.value().body,
                   std::string("data: from-test-1\n\ndata: from-test-2\n\n"));
    }
    ZEXPECT_TRUE(f.handlerOk.load());
}

ZTEST(Stream, ChunkedWriterBody) {
    StreamFixture f;
    auto res = f.fetch("/chunked");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);
    ZEXPECT_EQ(res.value().headers.get("Transfer-Encoding"), std::string("chunked"));
    ZEXPECT_EQ(res.value().body, std::string("AAABBBCCC"));
}

ZTEST(Stream, ChunkedFlagWithoutStreamStillTerminates) {
    StreamFixture f;
    auto res = f.fetch("/chunked-flag-only");
    ZEXPECT_TRUE(res.isOk()); // 关键：客户端不被挂住（能读到终止块）
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);
    ZEXPECT_TRUE(res.value().body.empty());
}

ZTEST(Stream, PlainResponseUnaffected) {
    StreamFixture f;
    auto res = f.fetch("/plain");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);
    ZEXPECT_EQ(res.value().body, std::string("plain-body"));
    ZEXPECT_EQ(res.value().headers.get("Content-Length"), std::string("10"));
    ZEXPECT_FALSE(res.value().headers.contains("Transfer-Encoding"));
}

ZTEST_MAIN
