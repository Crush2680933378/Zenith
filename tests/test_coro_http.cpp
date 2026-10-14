// 协程网络等待器端到端（仅 ZENITH_ENABLE_COROUTINES=ON 时编译）
#include <atomic>
#include <chrono>
#include <thread>

#include "ztest.hpp"

#include "zenith/coro/net_awaiters.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
struct CoroFixture {
    net::EventLoop serverLoop;
    net::EventLoopThreadPool io{1};
    http::HttpServer server{&serverLoop, &io};
    std::thread runner;

    CoroFixture() {
        io.start();
        server.router().get("/coro/hello", [](http::Context& c) {
            c.text(200, "hello from coro");
        });
        server.router().post("/coro/echo", [](http::Context& c) {
            http::json::JsonValue v = c.bodyJson();
            c.json(200, v);
        });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { serverLoop.loop(); });
        std::this_thread::sleep_for(100ms);
    }
    ~CoroFixture() {
        serverLoop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }
};
} // namespace

ZTEST(CoroHttp, CoAwaitFetch) {
    CoroFixture f;

    // EventLoop 运行在独立线程；协程在调用线程 syncWait，I/O 完成在 loop 线程 resume
    net::EventLoop loop;
    std::thread loopThread([&] { loop.loop(); });

    auto task = [&]() -> coro::Task<http::HttpResponse> {
        http::ClientRequest rq;
        rq.host = "127.0.0.1";
        rq.port = f.port();
        rq.path = "/coro/hello";
        rq.timeoutMs = 4000;
        co_return co_await coro::httpFetch(loop, rq);
    };

    const http::HttpResponse resp = coro::syncWait(task());
    ZEXPECT_EQ(resp.status, 200);
    ZEXPECT_EQ(resp.body, std::string("hello from coro"));

    // POST + JSON
    auto postTask = [&]() -> coro::Task<http::HttpResponse> {
        http::ClientRequest rq;
        rq.host = "127.0.0.1";
        rq.port = f.port();
        rq.method = http::HttpMethod::Post;
        rq.path = "/coro/echo";
        rq.body = R"({"ok":true,"v":42})";
        rq.timeoutMs = 4000;
        co_return co_await coro::httpFetch(loop, rq);
    };
    const http::HttpResponse resp2 = coro::syncWait(postTask());
    ZEXPECT_EQ(resp2.status, 200);
    auto v = http::json::JsonValue::parse(resp2.body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_TRUE(v.value()["ok"].asBool());
    ZEXPECT_EQ(v.value()["v"].asInt(), int64_t{42});

    loop.stop();
    loopThread.join();
}

ZTEST(CoroHttp, CoAwaitConnectFailureThrows) {
    net::EventLoop loop;
    std::thread loopThread([&] { loop.loop(); });

    bool threw = false;
    try {
        auto t = coro::tcpConnect(loop, "127.0.0.1", 1, 1200); // 不可达端口
        coro::syncWait(std::move(t));
    } catch (const std::runtime_error&) {
        threw = true;
    }
    ZEXPECT_TRUE(threw);

    loop.stop();
    loopThread.join();
}

ZTEST_MAIN
