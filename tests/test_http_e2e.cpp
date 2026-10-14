// HTTP 端到端测试：HttpServer + HttpClient(fetchBlocking) 真实回环通信
#include <atomic>
#include <chrono>
#include <thread>

#include "ztest.hpp"

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

/// 测试夹具：起一台本地服务器（临时端口 + IO 线程组）
struct HttpFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;

    HttpFixture() {
        io.start();
        auto& r = server.router();

        r.get("/hello", [](http::Context& c) { c.text(200, "hello world"); });

        r.get("/api/users/:id", [](http::Context& c) {
            http::json::JsonValue v = http::json::JsonValue::object();
            v["id"] = c.param("id");
            v["name"] = "zenith";
            v["verified"] = true;
            c.json(200, v);
        });

        r.post("/echo", [](http::Context& c) {
            http::json::JsonValue body = c.bodyJson(); // 非 JSON → 400
            http::json::JsonValue v = http::json::JsonValue::object();
            v["got"] = std::move(body);
            c.json(200, v);
        });

        r.use([](http::Context& c) {
            c.setHeader("X-Middleware", "yes");
            c.next();
        });

        r.get("/middleware-marked", [](http::Context& c) { c.text(200, "m"); });

        r.get("/boom", [](http::Context&) { throw std::runtime_error("kaboom"); });

        r.post("/only-post", [](http::Context& c) { c.text(201, "created"); });

        r.get("/query", [](http::Context& c) {
            c.text(200, "q=" + c.query("q", "<none>") + "&n=" + c.query("n"));
        });

        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms); // 监听生效
    }

    ~HttpFixture() {
        loop.stop();
        runner.join();
        io.stop();
    }

    [[nodiscard]] uint16_t port() const { return server.port(); }

    static http::ClientRequest makeReq(http::HttpMethod m, uint16_t port, std::string path,
                                       std::string body = "") {
        http::ClientRequest rq;
        rq.host = "127.0.0.1";
        rq.port = port;
        rq.method = m;
        rq.path = std::move(path);
        rq.body = std::move(body);
        rq.timeoutMs = 4000;
        return rq;
    }
};

} // namespace

ZTEST(HttpE2E, GetPlainText) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/hello"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().body, std::string("hello world"));
    ZEXPECT_EQ(r.value().headers.get("Content-Type"), std::string("text/plain; charset=utf-8"));
}

ZTEST(HttpE2E, GetJsonWithParam) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/api/users/42"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    auto v = http::json::JsonValue::parse(r.value().body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_EQ(v.value()["id"].asString(), std::string("42"));
    ZEXPECT_EQ(v.value()["name"].asString(), std::string("zenith"));
    ZEXPECT_TRUE(v.value()["verified"].asBool());
    ZEXPECT_EQ(r.value().headers.get("Content-Type"), std::string("application/json; charset=utf-8"));
}

ZTEST(HttpE2E, PostEchoJson) {
    HttpFixture f;
    const std::string body = R"({"msg":"你好","n":7,"arr":[1,2,3]})";
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Post, f.port(), "/echo", body));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    auto v = http::json::JsonValue::parse(r.value().body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_EQ(v.value()["got"]["msg"].asString(), std::string("你好"));
    ZEXPECT_EQ(v.value()["got"]["n"].asInt(), int64_t{7});
    ZEXPECT_EQ(v.value()["got"]["arr"].size(), size_t{3});
}

ZTEST(HttpE2E, MiddlewareAddsHeader) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/middleware-marked"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().headers.get("X-Middleware"), std::string("yes"));
}

ZTEST(HttpE2E, NotFoundAndMethodNotAllowed) {
    HttpFixture f;
    auto nf = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/definitely-missing"));
    ZEXPECT_TRUE(static_cast<bool>(nf));
    ZEXPECT_EQ(nf.value().status, 404);

    auto mna = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/only-post"));
    ZEXPECT_TRUE(static_cast<bool>(mna));
    ZEXPECT_EQ(mna.value().status, 405);
}

ZTEST(HttpE2E, HandlerExceptionYields500) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/boom"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 500);
}

ZTEST(HttpE2E, BadJsonBodyYields400) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Post, f.port(), "/echo", "this is not json"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 400);
}

ZTEST(HttpE2E, QueryParams) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/query?q=zen%20ith&n=5"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().body, std::string("q=zen ith&n=5"));

    auto d = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Get, f.port(), "/query"));
    ZEXPECT_EQ(d.value().body, std::string("q=<none>&n="));
}

ZTEST(HttpE2E, HeadRequest) {
    HttpFixture f;
    auto r = http::HttpClient::fetchBlocking(HttpFixture::makeReq(http::HttpMethod::Head, f.port(), "/hello"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_TRUE(r.value().body.empty()); // HEAD 无响应体
    // Content-Length 仍标示 GET 语义长度（serialize(false) 写 0，客户端解析为准）
}

ZTEST(HttpE2E, LargeBodyRoundtrip) {
    HttpFixture f;
    std::string big(200 * 1024, 'x'); // 200KB
    big[0] = '"';
    big.back() = '"';
    auto r = http::HttpClient::fetchBlocking(
        HttpFixture::makeReq(http::HttpMethod::Post, f.port(), "/echo", big));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    auto v = http::json::JsonValue::parse(r.value().body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_EQ(v.value()["got"].asString().size(), size_t{200 * 1024 - 2}); // 引号是分隔符
}

ZTEST(HttpE2E, ConnectTimeoutReported) {
    // 连接一个不存在服务的地址：127.0.0.1:1 会被拒收
    HttpFixture f; // 夹具端口无关紧要，仅复用类型
    (void)f;
    http::ClientRequest rq;
    rq.host = "127.0.0.1";
    rq.port = 1;
    rq.timeoutMs = 1500;
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_TRUE(r.error().message.find("connect failed") != std::string::npos);
}

ZTEST_MAIN
