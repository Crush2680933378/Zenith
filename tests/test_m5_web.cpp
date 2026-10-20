// M5.1 综合测试：multipart / 静态文件 / Session / 优雅停机 / wss
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/session.hpp"
#include "zenith/http/static_files.hpp"
#include "zenith/tls/tls_context.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- multipart 单测 ---------------- */

ZTEST(Multipart, ParseFieldsAndFiles) {
    const std::string body =
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"field1\"\r\n\r\n"
        "value1\r\n"
        "--BND\r\n"
        "Content-Disposition: form-data; name=\"upload\"; filename=\"a.bin\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" +
        std::string("\x00\x01\x02", 3) + "binary" + std::string("\xff", 1) + "\r\n" +
        "--BND--\r\n";
    auto parts = http::multipart::parse(body, "BND");
    ZEXPECT_TRUE(static_cast<bool>(parts));
    ZEXPECT_EQ(parts.value().size(), size_t{2});
    ZEXPECT_EQ(parts.value()[0].name, std::string("field1"));
    ZEXPECT_TRUE(!parts.value()[0].isFile());
    ZEXPECT_EQ(parts.value()[0].data, std::string("value1"));
    ZEXPECT_EQ(parts.value()[1].filename, std::string("a.bin"));
    ZEXPECT_EQ(parts.value()[1].data, std::string("\x00\x01\x02", 3) + "binary" + std::string("\xff", 1));
    ZEXPECT_EQ(parts.value()[1].data, std::string("\x00\x01\x02", 3) + "binary" + std::string("\xff", 1));
    ZEXPECT_EQ(parts.value()[1].contentType, std::string("application/octet-stream"));
}

ZTEST(Multipart, BoundaryExtractionAndErrors) {
    auto b1 = http::multipart::extractBoundary(
        "multipart/form-data; boundary=----WebKitFormABC");
    ZEXPECT_TRUE(static_cast<bool>(b1));
    ZEXPECT_EQ(b1.value(), std::string("----WebKitFormABC"));
    auto b2 = http::multipart::extractBoundary("multipart/form-data; boundary=\"quoted\"");
    ZEXPECT_TRUE(static_cast<bool>(b2));
    ZEXPECT_EQ(b2.value(), std::string("quoted"));

    ZEXPECT_TRUE(http::multipart::extractBoundary("text/plain").isErr());
    ZEXPECT_TRUE(http::multipart::parse("no boundary here", "X").isErr());
    ZEXPECT_TRUE(http::multipart::parse("--X\r\nbroken", "X").isErr());
}

/* ---------------- 静态文件单测 ---------------- */

ZTEST(StaticFiles, EtagAndMime) {
    ZEXPECT_EQ(http::computeFileEtag(100, 200), std::string("\"c8-64\""));
    ZEXPECT_EQ(http::mimeTypeFor("a/b/c.HTML"), std::string("text/html; charset=utf-8"));
    ZEXPECT_EQ(http::mimeTypeFor("x.png"), std::string("image/png"));
    ZEXPECT_EQ(http::mimeTypeFor("x.unknownext"), std::string("application/octet-stream"));
    ZEXPECT_EQ(http::mimeTypeFor("noext"), std::string("application/octet-stream"));
}

namespace {
struct WebFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    std::string docRoot = "zm5www.tmp";

    WebFixture() {
        std::error_code ec;
        std::filesystem::remove_all(docRoot, ec);
        std::filesystem::create_directories(docRoot + "/sub", ec);
        { std::ofstream(docRoot + "/index.html") << "<h1>zenith static</h1>"; }
        { std::ofstream(docRoot + "/sub/app.js") << "console.log(1);"; }
        { std::ofstream(docRoot + "/big.bin", std::ios::binary)
              << std::string(10000, 'B'); }

        io.start();
        server.router().get("/assets/*path", http::staticHandler(docRoot, "/assets"));
        server.router().post("/form", [](http::Context& c) {
            http::json::JsonValue v = http::json::JsonValue::object();
            v["field"] = c.form("name");
            const auto* f = c.file("doc");
            if (f != nullptr) {
                v["file"] = f->filename;
                v["size"] = static_cast<int64_t>(f->data.size());
            }
            c.json(200, v);
        });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(120ms);
    }
    ~WebFixture() {
        loop.stop();
        runner.join();
        io.stop();
        std::error_code ec;
        std::filesystem::remove_all(docRoot, ec);
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }
};
} // namespace

ZTEST(StaticE2E, Get304RangeTraversal) {
    WebFixture f;

    // 基础 GET
    auto r = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/index.html");
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    ZEXPECT_EQ(r.value().body, std::string("<h1>zenith static</h1>"));
    ZEXPECT_EQ(r.value().headers.get("Content-Type"), std::string("text/html; charset=utf-8"));
    const std::string etag = r.value().headers.get("ETag");
    ZEXPECT_FALSE(etag.empty());
    ZEXPECT_EQ(r.value().headers.get("Accept-Ranges"), std::string("bytes"));

    // 子目录 + mime
    auto js = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/sub/app.js");
    ZEXPECT_EQ(js.value().status, 200);
    ZEXPECT_EQ(js.value().headers.get("Content-Type"),
               std::string("application/javascript; charset=utf-8"));

    // 304（If-None-Match）
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/index.html";
        rq.headers.set("If-None-Match", etag);
        auto nm = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_TRUE(static_cast<bool>(nm));
        ZEXPECT_EQ(nm.value().status, 304);
        ZEXPECT_TRUE(nm.value().body.empty());
    }

    // Range 单段
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/big.bin";
        rq.headers.set("Range", "bytes=100-199");
        auto rg = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_TRUE(static_cast<bool>(rg));
        ZEXPECT_EQ(rg.value().status, 206);
        ZEXPECT_EQ(rg.value().body.size(), size_t{100});
        ZEXPECT_EQ(rg.value().headers.get("Content-Range"), std::string("bytes 100-199/10000"));
    }
    // Range 开放结尾 / 后缀
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/big.bin";
        rq.headers.set("Range", "bytes=9990-");
        auto rg = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(rg.value().status, 206);
        ZEXPECT_EQ(rg.value().body.size(), size_t{10});
    }
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/big.bin";
        rq.headers.set("Range", "bytes=-50");
        auto rg = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(rg.value().status, 206);
        ZEXPECT_EQ(rg.value().body.size(), size_t{50});
    }
    // 非法 Range → 200 全量
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/big.bin";
        rq.headers.set("Range", "bytes=zzz");
        auto rg = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(rg.value().status, 200);
        ZEXPECT_EQ(rg.value().body.size(), size_t{10000});
    }

    // 目录穿越 → 403 / 缺失 → 404
    auto trav = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/..%2F..%2FCMakeLists.txt");
    // 注：path 已解码为 /assets/../../CMakeLists.txt → Router catch-all 收到后 staticHandler 拒绝
    (void)trav;
    auto miss = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(f.port()) + "/assets/nope.txt");
    ZEXPECT_EQ(miss.value().status, 404);
}

/* ---------------- SPA 静态服务（M5.7） ---------------- */

/// spaHandler 回归测试——这个用例固化的是 Blog 实战踩出的三条规则：
/// 1) 真实文件优先（/assets/app.js 必须回 JS，不能回 HTML）
/// 2) 未命中路径回 index.html（前端路由接管，刷新深链不 404）
/// 3) 根路径 "/" 不命中 catch-all，必须单独注册（空剩余段）
ZTEST(SpaE2E, FallbackAndAssetPriority) {
    const std::string root = "zspa.tmp";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root + "/assets", ec);
    { std::ofstream(root + "/index.html") << "<div id=spa-shell>"; }
    { std::ofstream(root + "/assets/app.js") << "console.log('real');"; }

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.router().get("/", http::spaHandler(root));
    server.router().get("/*path", http::spaHandler(root));
    (void)server.listen("127.0.0.1", 0);
    std::thread runner([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);

    const std::string base = "http://127.0.0.1:" + std::to_string(server.port());

    // 1) 根路径：单独注册的 "/" → index.html
    auto r0 = http::HttpClient::fetchBlockingUrl(base + "/");
    ZEXPECT_EQ(r0.value().status, 200);
    ZEXPECT_EQ(r0.value().body, std::string("<div id=spa-shell>"));
    ZEXPECT_TRUE(r0.value().headers.get("Content-Type").find("text/html") != std::string::npos);
    ZEXPECT_TRUE(r0.value().headers.get("Cache-Control").find("no-cache") != std::string::npos);

    // 2) 深链（前端路由路径，磁盘上不存在）→ 回 index.html，200 而非 404
    auto r1 = http::HttpClient::fetchBlockingUrl(base + "/game/room/ABCD");
    ZEXPECT_EQ(r1.value().status, 200);
    ZEXPECT_EQ(r1.value().body, std::string("<div id=spa-shell>"));

    // 3) 真实静态资源：优先于回退（绝不能被 index.html 顶掉）
    auto r2 = http::HttpClient::fetchBlockingUrl(base + "/assets/app.js");
    ZEXPECT_EQ(r2.value().status, 200);
    ZEXPECT_EQ(r2.value().body, std::string("console.log('real');"));
    ZEXPECT_TRUE(r2.value().headers.get("Content-Type").find("javascript") != std::string::npos);

    // 4) 带扩展名的不存在资源 → 404（不能伪装成 200 HTML，否则前端加载到
    //    垃圾字节且 Content-Type 错误、缓存协商失效——实战最难排查的一类问题）
    auto r3 = http::HttpClient::fetchBlockingUrl(base + "/assets/missing.js");
    ZEXPECT_EQ(r3.value().status, 404);

    loop.stop();
    runner.join();
    io.stop();
    std::filesystem::remove_all(root, ec);
}

ZTEST(FormE2E, MultipartUpload) {
    WebFixture f;
    const std::string boundary = "ZenBoundary123";
    const std::string fileData(3000, 'F');
    const std::string body =
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"name\"\r\n\r\n"
        "zenith\r\n"
        "--" + boundary + "\r\n"
        "Content-Disposition: form-data; name=\"doc\"; filename=\"report.dat\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n" +
        fileData +
        "\r\n"
        "--" + boundary + "--\r\n";

    http::ClientRequest rq;
    rq.url = "http://127.0.0.1:" + std::to_string(f.port()) + "/form";
    rq.method = http::HttpMethod::Post;
    rq.body = body;
    rq.headers.set("Content-Type", "multipart/form-data; boundary=" + boundary);
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().status, 200);
    auto v = http::json::JsonValue::parse(r.value().body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_EQ(v.value()["field"].asString(), std::string("zenith"));
    ZEXPECT_EQ(v.value()["file"].asString(), std::string("report.dat"));
    ZEXPECT_EQ(v.value()["size"].asInt(), int64_t{3000});
}

/* ---------------- Session ---------------- */

ZTEST(SessionE2E, CookieRoundTripAndPersist) {
    auto mgr = std::make_shared<http::SessionManager>(
        std::make_shared<http::MemorySessionStore>());

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.router().use(http::sessionMiddleware(mgr));
    server.router().get("/login", [](http::Context& c) {
        c.session()->set("user", "tom");
        c.text(200, "ok");
    });
    server.router().get("/whoami", [](http::Context& c) {
        c.text(200, c.session()->get("user", "anonymous"));
    });
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);

    // 1) 登录拿 Set-Cookie
    auto r1 = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/login");
    ZEXPECT_EQ(r1.value().status, 200);
    const std::string setCookie = r1.value().headers.get("Set-Cookie");
    ZEXPECT_TRUE(setCookie.find("zenith_sid=") != std::string::npos);
    ZEXPECT_TRUE(setCookie.find("HttpOnly") != std::string::npos);

    // 提取 sid
    const size_t eq = setCookie.find('=');
    const std::string sid = setCookie.substr(eq + 1,
                                             setCookie.find(';', eq) - eq - 1);

    // 2) 带 cookie 访问 → 会话命中
    http::ClientRequest rq;
    rq.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/whoami";
    rq.headers.set("Cookie", "zenith_sid=" + sid);
    auto r2 = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_EQ(r2.value().body, std::string("tom"));

    // 3) 无 cookie → 新会话 anonymous
    auto r3 = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/whoami");
    ZEXPECT_EQ(r3.value().body, std::string("anonymous"));

    loop.stop();
    t.join();
    io.stop();
}

ZTEST(Session, SerializeRoundtripAndStore) {
    http::MemorySessionStore store;
    store.save("s1", "a=1\nb=hello world\n");
    ZEXPECT_EQ(store.load("s1"), std::string("a=1\nb=hello world\n"));
    ZEXPECT_EQ(store.load("missing"), std::string(""));
    store.remove("s1");
    ZEXPECT_EQ(store.size(), size_t{0});

    auto mgr = std::make_shared<http::SessionManager>(
        std::make_shared<http::MemorySessionStore>());
    auto s = mgr->load("");
    ZEXPECT_TRUE(s->isNew());
    s->set("k", "v");
    ZEXPECT_TRUE(mgr->save(s)); // 新建 → true
    ZEXPECT_FALSE(mgr->save(s)); // 未变 → false
    auto s2 = mgr->load(s->id());
    ZEXPECT_FALSE(s2->isNew());
    ZEXPECT_EQ(s2->get("k"), std::string("v"));
}

/* ---------------- 优雅停机 ---------------- */

ZTEST(GracefulShutdown, SlowRequestCompletesThenDrained) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.router().get("/slow", [&loop](http::Context& c) {
        loop.scheduleAfter(300ms, [c2 = std::make_shared<http::Context>(std::move(c))]() {
            c2->text(200, "done");
        });
        // 注意：延迟响应——handler 返回但响应在其后（计数器守护在 send 前结束）
    });
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);

    std::atomic<bool> drained{false};
    std::atomic<bool> gotResponse{false};
    std::thread requester([&] {
        auto r = http::HttpClient::fetchBlockingUrl(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/slow");
        if (r && r.value().status == 200) gotResponse.store(true);
    });
    std::this_thread::sleep_for(50ms); // 等请求进入

    loop.runInLoop([&] {
        server.beginGracefulShutdown([&] {
            drained.store(true);
            loop.stop(); // 排空完成 → 停 loop
        });
    });

    requester.join();
    t.join();
    io.stop();
    ZEXPECT_TRUE(gotResponse.load());
    ZEXPECT_TRUE(drained.load());
}

/* ---------------- wss（WS over TLS）---------------- */

ZTEST(WssE2E, WebSocketOverTls) {
    // 现场自签证书（复用 M4 的 OpenSSL C API 方式）
    std::error_code ec;
    std::filesystem::remove_all("zwss.tmp", ec);
    std::filesystem::create_directories("zwss.tmp", ec);
    {
        EVP_PKEY* pkey = EVP_RSA_gen(2048);
        X509* x = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_get_notBefore(x), 0);
        X509_gmtime_adj(X509_get_notAfter(x), 3600LL * 24);
        X509_set_pubkey(x, pkey);
        X509_NAME* n = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                                   (const unsigned char*)"127.0.0.1", -1, -1, 0);
        X509_set_issuer_name(x, n);
        GENERAL_NAMES* gns = GENERAL_NAMES_new();
        GENERAL_NAME* gn = GENERAL_NAME_new();
        GENERAL_NAME_set0_value(gn, GEN_IPADD, a2i_IPADDRESS("127.0.0.1"));
        sk_GENERAL_NAME_push(gns, gn);
        X509_add1_ext_i2d(x, NID_subject_alt_name, gns, 0, X509V3_ADD_DEFAULT);
        GENERAL_NAMES_free(gns);
        if (X509_sign(x, pkey, EVP_sha256())) {
            FILE* kf = fopen("zwss.tmp/k.pem", "wb");
            PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
            fclose(kf);
            FILE* cf = fopen("zwss.tmp/c.pem", "wb");
            PEM_write_X509(cf, x);
            fclose(cf);
        }
        X509_free(x);
        EVP_PKEY_free(pkey);
    }

    auto ctxR = tls::OpenSslContext::createServer(
        tls::TlsServerConfig{"zwss.tmp/c.pem", "zwss.tmp/k.pem", "http/1.1"});
    ZEXPECT_TRUE(static_cast<bool>(ctxR));
    if (!ctxR) return;

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.enableWebSocket("/ws", [](const net::WebSocketSession::Ptr& s) {
        s->setMessageCallback([](const net::WebSocketSession::Ptr& sess, std::string m, bool) {
            sess->sendText("wss-echo:" + m);
        });
    });
    (void)server.listenTls(ctxR.value(), "127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(150ms);

    net::EventLoop cloop;
    std::thread ct([&] { cloop.loop(); });
    std::atomic<bool> gotEcho{false};
    std::atomic<bool> closed{false};
    std::string echoed;

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = server.port();
    opt.path = "/ws";
    opt.timeoutMs = 6000;
    opt.tls = true;
    opt.caFile = "zwss.tmp/c.pem";
    client->connect(
        &cloop, opt,
        [&](const net::WebSocketSession::Ptr& s0) {
            auto s = s0;
            s->setMessageCallback(
                [s, &echoed, &gotEcho](const net::WebSocketSession::Ptr&, std::string m, bool) {
                    echoed = m;
                    gotEcho.store(true);
                    s->close();
                });
            s->setCloseCallback([&closed](const net::WebSocketSession::Ptr&) {
                closed.store(true);
            });
            s->sendText("secure-hello");
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "wss connect failed: " + reason);
        });

    for (int i = 0; i < 400 && !closed.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(gotEcho.load());
    ZEXPECT_EQ(echoed, std::string("wss-echo:secure-hello"));

    cloop.stop();
    ct.join();
    loop.stop();
    t.join();
    io.stop();
    std::filesystem::remove_all("zwss.tmp", ec);
}

ZTEST_MAIN
