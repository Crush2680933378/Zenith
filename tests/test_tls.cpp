// TLS 端到端测试（ZENITH_HAS_TLS 时编译）：
// openssl CLI 生成自签证书 → HttpsServer → (a) Zenith 客户端 https 校验访问
// (b) 证书不匹配/未信任失败路径 (c) keep-alive 连接池复用
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>

#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "ztest.hpp"

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

#ifndef ZENITH_HAS_TLS
ZTEST(Tls, SkippedNoBackend) { ZEXPECT_TRUE(true); }
ZTEST_MAIN
#else

namespace {
/// 用 OpenSSL C API 直接生成自签证书（CN/SAN=127.0.0.1）——不依赖 CLI
struct CertDir {
    std::string dir = "ztls_certs.tmp";
    std::string key = "ztls_certs.tmp/server.key";
    std::string cert = "ztls_certs.tmp/server.crt";
    bool ok_ = false;

    CertDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);

        EVP_PKEY* pkey = EVP_RSA_gen(2048);
        if (pkey == nullptr) return;
        X509* x = X509_new();
        if (x == nullptr) {
            EVP_PKEY_free(pkey);
            return;
        }
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_get_notBefore(x), 0);
        X509_gmtime_adj(X509_get_notAfter(x), 3600LL * 24 * 2);
        X509_set_pubkey(x, pkey);

        X509_NAME* name = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1,
                                   -1, 0);
        X509_set_issuer_name(x, name);
        // SAN = IP:127.0.0.1（现代校验必需）
        GENERAL_NAMES* gns = GENERAL_NAMES_new();
        GENERAL_NAME* gn = GENERAL_NAME_new();
        ASN1_OCTET_STRING* ip = a2i_IPADDRESS("127.0.0.1");
        GENERAL_NAME_set0_value(gn, GEN_IPADD, ip);
        sk_GENERAL_NAME_push(gns, gn);
        X509_add1_ext_i2d(x, NID_subject_alt_name, gns, 0, X509V3_ADD_DEFAULT);
        GENERAL_NAMES_free(gns);

        ok_ = X509_sign(x, pkey, EVP_sha256()) != 0;
        if (ok_) {
            FILE* kf = std::fopen(key.c_str(), "wb");
            if (kf != nullptr) {
                PEM_write_PrivateKey(kf, pkey, nullptr, nullptr, 0, nullptr, nullptr);
                std::fclose(kf);
            }
            FILE* cf = std::fopen(cert.c_str(), "wb");
            if (cf != nullptr) {
                PEM_write_X509(cf, x);
                std::fclose(cf);
            }
        }
        X509_free(x);
        EVP_PKEY_free(pkey);
    }
    ~CertDir() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    [[nodiscard]] bool ok() const { return ok_; }
};

struct HttpsFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    tls::OpenSslContext ctx;

    explicit HttpsFixture(const CertDir& certs) {
        io.start();
        auto ctxR = tls::OpenSslContext::createServer(
            tls::TlsServerConfig{certs.cert, certs.key, "http/1.1"});
        if (!ctxR) return;
        ctx = std::move(ctxR).value();

        server.router().get("/hello", [](http::Context& c) { c.text(200, "secure hello"); });
        server.router().post("/echo", [](http::Context& c) {
            c.json(200, http::json::JsonValue::parse(c.body()).value());
        });
        (void)server.listenTls(ctx, "127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(150ms);
    }
    ~HttpsFixture() {
        loop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }
};
} // namespace

ZTEST(TlsE2E, HttpsRoundtripWithTrust) {
    CertDir certs;
    if (!certs.ok()) {
        ztest::reportFail(__FILE__, __LINE__, "openssl CLI 生成证书失败（跳过前失败提示环境问题）");
        return;
    }
    HttpsFixture f(certs);
    if (!f.ctx.valid()) {
        ztest::reportFail(__FILE__, __LINE__, "服务端 TLS 上下文创建失败");
        return;
    }

    http::ClientRequest rq;
    rq.url = "https://127.0.0.1:" + std::to_string(f.port()) + "/hello";
    rq.caFile = certs.cert;    // 信任我们的自签 CA
    rq.timeoutMs = 6000;
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    if (r) {
        ZEXPECT_EQ(r.value().status, 200);
        ZEXPECT_EQ(r.value().body, std::string("secure hello"));
    }
}

ZTEST(TlsE2E, UntrustedCertRejected) {
    CertDir certs;
    if (!certs.ok()) return;
    HttpsFixture f(certs);

    http::ClientRequest rq;
    rq.url = "https://127.0.0.1:" + std::to_string(f.port()) + "/hello";
    rq.caFile.clear();
    // 不提供 CA，仅系统信任
    rq.verifyPeer = true; // 不提供 CA → 校验失败
    rq.timeoutMs = 6000;
    auto r = http::HttpClient::fetchBlocking(rq);
    // 结果：握手失败（连接错误）——统一 Err
    ZEXPECT_TRUE(r.isErr());
}

ZTEST(TlsE2E, PostJsonOverTls) {
    CertDir certs;
    if (!certs.ok()) return;
    HttpsFixture f(certs);

    http::ClientRequest rq;
    rq.url = "https://127.0.0.1:" + std::to_string(f.port()) + "/echo";
    rq.method = http::HttpMethod::Post;
    rq.body = R"({"v":42,"ok":true})";
    rq.caFile = certs.cert;
    rq.timeoutMs = 6000;
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    if (r) {
        auto v = http::json::JsonValue::parse(r.value().body);
        ZEXPECT_TRUE(static_cast<bool>(v));
        ZEXPECT_EQ(v.value()["v"].asInt(), int64_t{42});
        ZEXPECT_TRUE(v.value()["ok"].asBool());
    }
}

ZTEST(TlsE2E, KeepAlivePoolReuse) {
    CertDir certs;
    if (!certs.ok()) return;
    HttpsFixture f(certs);

    net::EventLoop loop;
    std::thread t([&] { loop.loop(); });
    http::HttpClient client(&loop);

    // 串行链：下一次在上一次完成后发起（rq 按值捕获——异步任务跨越其生命期）
    std::atomic<int> done{0};
    std::function<void()> sendNext;
    sendNext = [&]() {
        if (done.load() >= 10) return;
        http::ClientRequest rq;
        rq.url = "https://127.0.0.1:" + std::to_string(f.port()) + "/hello";
        rq.caFile = certs.cert;
        rq.keepAlive = true;
        rq.timeoutMs = 6000;
        loop.queueInLoop([&client, rq, &done, &sendNext]() {
            client.request(rq, [&done, &sendNext](core::Result<http::HttpResponse> r) {
                if (r && r.value().status == 200) done.fetch_add(1);
                sendNext();
            });
        });
    };
    sendNext();
    for (int i = 0; i < 800 && done.load() < 10; ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_EQ(done.load(), 10);
    // 复用生效：池中空闲连接远少于请求数
    ZEXPECT_TRUE(client.pooledConnections() >= 1);
    ZEXPECT_TRUE(client.pooledConnections() < 10);
    client.clearPool();
    loop.stop();
    t.join();
}

ZTEST(UrlParser, Forms) {
    auto a = http::parseUrl("http://example.com");
    ZEXPECT_TRUE(static_cast<bool>(a));
    ZEXPECT_EQ(a.value().host, std::string("example.com"));
    ZEXPECT_EQ(a.value().port, uint16_t{80});
    ZEXPECT_FALSE(a.value().tls);
    ZEXPECT_EQ(a.value().pathAndQuery, std::string("/"));

    auto b = http::parseUrl("https://api.x.com:8443/v1/users?id=7");
    ZEXPECT_TRUE(static_cast<bool>(b));
    ZEXPECT_EQ(b.value().scheme, std::string("https"));
    ZEXPECT_TRUE(b.value().tls);
    ZEXPECT_EQ(b.value().port, uint16_t{8443});
    ZEXPECT_EQ(b.value().pathAndQuery, std::string("/v1/users?id=7"));

    auto c = http::parseUrl("localhost:8080/x");
    ZEXPECT_TRUE(static_cast<bool>(c));
    ZEXPECT_EQ(c.value().host, std::string("localhost"));
    ZEXPECT_EQ(c.value().port, uint16_t{8080});

    ZEXPECT_TRUE(http::parseUrl("ftp://x/").isErr());
    ZEXPECT_TRUE(http::parseUrl("http://").isErr());
    ZEXPECT_TRUE(http::parseUrl("http://h:port/").isErr());
}

ZTEST(HttpPool, PlainHttpKeepAlive) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    server.router().get("/ping", [](http::Context& c) { c.text(200, "pong"); });
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);

    net::EventLoop cloop;
    std::thread ct([&] { cloop.loop(); });
    http::HttpClient client(&cloop);

    // 串行链式请求：下一次在上一次完成后发起——验证池取回复用
    std::atomic<int> done{0};
    std::function<void()> sendNext;
    sendNext = [&]() {
        if (done.load() >= 20) return;
        http::ClientRequest rq;
        rq.host = "127.0.0.1";
        rq.port = server.port();
        rq.path = "/ping";
        rq.keepAlive = true;
        rq.timeoutMs = 4000;
        cloop.queueInLoop([&client, rq, &done, &sendNext]() { // rq 按值（异步任务跨越其生命期）
            client.request(rq, [&done, &sendNext](core::Result<http::HttpResponse> r) {
                if (r && r.value().status == 200) done.fetch_add(1);
                sendNext();
            });
        });
    };
    sendNext();
    for (int i = 0; i < 800 && done.load() < 20; ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_EQ(done.load(), 20);
    ZEXPECT_TRUE(client.pooledConnections() >= 1);   // 有连接归还池
    ZEXPECT_TRUE(client.pooledConnections() < 20);   // 且明显少于请求数（复用生效）
    client.clearPool();

    cloop.stop(); ct.join();
    loop.stop(); t.join();
    io.stop();
}

ZTEST_MAIN
#endif
