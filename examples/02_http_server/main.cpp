// ============================================================================
//  02_http_server —— Gin 风格 HTTP 服务示例
//  演示：路由参数 / 查询 / JSON / 中间件 / 分组 / 异常兜底
//  运行后：curl http://127.0.0.1:8080/hello
// ============================================================================
#include <chrono>
#include <iostream>
#include <thread>

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"

int main() {
    using namespace zenith;

    auto& facade = log::LoggerFacade::instance();
    facade.setLevel(log::LogLevel::Debug);
    facade.addSink(std::make_shared<log::ConsoleLogger>(true, true));

    net::EventLoop loop;
    net::EventLoopThreadPool ioPool(4);
    ioPool.start();
    http::HttpServer server(&loop, &ioPool);

    auto& r = server.router();

    // 访问日志中间件：洋葱模型（后置记录耗时）
    r.use([](http::Context& c) {
        const int64_t t0 = core::steadyMicroseconds();
        c.next();
        ZLOG_INFO(c.peer(), " ", http::toString(c.method()), " ", c.path(),
                  " -> ", c.responseStatus(), " (", (core::steadyMicroseconds() - t0) / 1000,
                  "ms)");
    });

    r.get("/hello", [](http::Context& c) { c.text(200, "Hello, Zenith!"); });

    r.get("/api/users/:id", [](http::Context& c) {
        http::json::JsonValue v = http::json::JsonValue::object();
        v["id"] = c.param("id");
        v["name"] = "zenith";
        v["tags"] = [] {
            http::json::JsonValue arr = http::json::JsonValue::array();
            arr.push_back("cpp");
            arr.push_back("web");
            return arr;
        }();
        c.json(200, v);
    });

    r.post("/api/echo", [](http::Context& c) {
        http::json::JsonValue in = c.bodyJson();
        in["echoed_by"] = "zenith";
        c.json(200, in);
    });

    // API v1 分组 + 组级中间件
    auto v1 = r.group("/api/v1");
    v1.use([](http::Context& c) {
        if (c.header("Authorization").empty()) {
            c.abort(401, "missing Authorization");
            return;
        }
        c.next();
    });
    v1.get("/secret", [](http::Context& c) { c.text(200, "top secret"); });

    // 405 演示
    r.post("/only-post", [](http::Context& c) { c.text(201, "created"); });

    auto lr = server.listen("0.0.0.0", 8080);
    if (!lr) {
        ZLOG_ERROR("listen failed: ", lr.error().message);
        return 1;
    }
    ZLOG_INFO("Zenith HTTP server listening on 127.0.0.1:8080 (",
              server.port(), "), io threads = ", ioPool.size());

    // 演示自请求（3 秒后，独立线程——fetchBlocking 会阻塞所在线程，
    // 不能放在主 loop 线程上，否则 accept 被卡死导致自死锁）
    loop.scheduleAfter(std::chrono::seconds(3), [&server] {
        std::thread fetcher([&server] {
            http::ClientRequest rq;
            rq.host = "127.0.0.1";
            rq.port = server.port();
            rq.path = "/api/users/42";
            rq.timeoutMs = 5000;
            auto r2 = http::HttpClient::fetchBlocking(rq);
            ZLOG_INFO("self-fetch /api/users/42 -> ",
                      r2 ? std::to_string(r2.value().status) : r2.error().message, " ",
                      r2 ? r2.value().body : std::string());
        });
        fetcher.detach();
    });

    loop.loop(); // 阻塞运行（Ctrl+C 退出由后续里程碑的信号处理补齐）

    ioPool.stop();
    facade.flush();
    facade.shutdown();
    return 0;
}
