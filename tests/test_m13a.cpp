// M13 第一批测试：TLS https 路由 / 向量持久化 / 三协议流式 SSE 翻译
#include <chrono>
#include <filesystem>
#include <thread>

#include <openssl/evp.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "ztest.hpp"

#include "zenith/ai/provider_router.hpp"
#include "zenith/crew/crew_dashboard.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/sse.hpp"
#include "zenith/tls/tls_context.hpp"
#include "zenith/vector/vector_persist.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- M13.1 https(TLS) 路由 ---------------- */

namespace {
/// 生成自签证书的极简工具（复用既有 OpenSSL 直调模式）
struct TmpCert {
    std::string dir = "zm13tls.tmp";
    std::string key = "zm13tls.tmp/k.pem";
    std::string cert = "zm13tls.tmp/c.pem";
    bool ok = false;

    TmpCert() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        std::filesystem::create_directories(dir, ec);
        FILE* kf = nullptr;
        FILE* cf = nullptr;
        EVP_PKEY* pk = EVP_RSA_gen(2048);
        X509* x = X509_new();
        ASN1_INTEGER_set(X509_get_serialNumber(x), 1);
        X509_gmtime_adj(X509_get_notBefore(x), 0);
        X509_gmtime_adj(X509_get_notAfter(x), 3600LL * 24);
        X509_set_pubkey(x, pk);
        X509_NAME* n = X509_get_subject_name(x);
        X509_NAME_add_entry_by_txt(n, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>("127.0.0.1"), -1,
                                   -1, 0);
        X509_set_issuer_name(x, n);
        GENERAL_NAMES* gns = GENERAL_NAMES_new();
        GENERAL_NAME* gn = GENERAL_NAME_new();
        GENERAL_NAME_set0_value(gn, GEN_IPADD, a2i_IPADDRESS("127.0.0.1"));
        sk_GENERAL_NAME_push(gns, gn);
        X509_add1_ext_i2d(x, NID_subject_alt_name, gns, 0, X509V3_ADD_DEFAULT);
        GENERAL_NAMES_free(gns);
        ok = X509_sign(x, pk, EVP_sha256()) != 0;
        if (ok) {
            kf = std::fopen(key.c_str(), "wb");
            PEM_write_PrivateKey(kf, pk, nullptr, nullptr, 0, nullptr, nullptr);
            std::fclose(kf);
            cf = std::fopen(cert.c_str(), "wb");
            PEM_write_X509(cf, x);
            std::fclose(cf);
        }
        X509_free(x);
        EVP_PKEY_free(pk);
    }
    ~TmpCert() {
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
};
} // namespace

#ifdef ZENITH_TLS_OPENSSL
ZTEST(RouterTls, HttpsDirectToSelfSignedServer) {
    TmpCert tc;
    ZEXPECT_TRUE(tc.ok);

    // 自家 TLS 服务器（openai 形态 mock）
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    auto ctx = tls::OpenSslContext::createServer(tls::TlsServerConfig{tc.cert, tc.key, ""});
    ZEXPECT_TRUE(static_cast<bool>(ctx));
    http::HttpServer srv(&loop, &io);
    srv.router().post("/v1/chat/completions", [](http::Context& c) {
        json::JsonValue out = json::JsonValue::object();
        json::JsonValue choice = json::JsonValue::object();
        choice["message"]["content"] = "via-tls";
        json::JsonValue choices = json::JsonValue::array();
        choices.push_back(std::move(choice));
        out["choices"] = std::move(choices);
        out["usage"]["prompt_tokens"] = int64_t{2};
        c.json(200, out);
    });
    (void)srv.listenTls(ctx.value(), "127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    // https 路由（insecure 跳过自签校验）
    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"local","api":"openai-chat","baseURL":"https://127.0.0.1:)")
        + std::to_string(srv.port())
        + R"(/v1","apiKey":"k","insecure":true,"models":["m"]}]})").value();
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req);
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_EQ(r.content, std::string("via-tls"));
    ZEXPECT_EQ(r.usage.promptTokens, 2);

    loop.stop();
    t.join();
    io.stop();
}

ZTEST(RouterTls, VerifiedCertRejectedWhenNotInsecure) {
    TmpCert tc;
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    auto ctx = tls::OpenSslContext::createServer(tls::TlsServerConfig{tc.cert, tc.key, ""});
    http::HttpServer srv(&loop, &io);
    srv.router().post("/v1/chat/completions", [](http::Context& c) { c.json(200, json::JsonValue()); });
    (void)srv.listenTls(ctx.value(), "127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"local","api":"openai-chat","baseURL":"https://127.0.0.1:)")
        + std::to_string(srv.port())
        + R"(/v1","apiKey":"k","models":["m"]}]})").value(); // 无 insecure → 校验自签必失败
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req);
    ZEXPECT_FALSE(r.ok); // 证书不受信 → 握手失败 → 明确报错
    ZEXPECT_TRUE(r.error.find("tls") != std::string::npos);

    loop.stop();
    t.join();
    io.stop();
}
#else
ZTEST(RouterTls, SkippedNoOpenSsl) { ZEXPECT_TRUE(true); }
#endif

/* ---------------- M13.2 向量持久化 ---------------- */

ZTEST(VectorPersist, SnapshotRoundtrip) {
    const std::string dir = "zm13vec.tmp";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    vector::HnswConfig cfg;
    cfg.dim = 4;
    cfg.metric = vector::Metric::Cosine;
    vector::HnswIndex idx(cfg);
    float a[4] = {1, 0, 0, 0};
    float b[4] = {0, 1, 0, 0};
    idx.upsert(1, a);
    idx.upsert(2, b);

    // 保存 → 重新加载 → 同检索结果
    ZEXPECT_TRUE(static_cast<bool>(vector::saveCollection(dir, "docs", idx)));
    auto back = vector::loadCollection(dir, "docs");
    ZEXPECT_TRUE(static_cast<bool>(back));
    ZEXPECT_EQ(back.value()->size(), size_t{2});
    auto hits = back.value()->search(a, 2);
    ZEXPECT_EQ(hits.size(), size_t{2});
    ZEXPECT_EQ(hits[0].id, static_cast<uint64_t>(1));

    // 列举 + 原子替换语义（再存同名为 .tmp → rename）
    ZEXPECT_TRUE(static_cast<bool>(vector::saveCollection(dir, "docs", idx)));
    auto names = vector::listCollections(dir);
    ZEXPECT_EQ(names.size(), size_t{1});
    ZEXPECT_EQ(names[0], std::string("docs"));

    std::filesystem::remove_all(dir, ec);
}

/* ---------------- M13.3 三协议流式 SSE 翻译 ---------------- */

namespace {
/// 起一个本地 SSE mock 服务器：method path → 事件 data 列表（含 [DONE] 语义）
struct SseMock {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    http::HttpServer srv{&loop, &io};
    std::thread t;
    std::vector<std::string> seenBodies; // 收到的请求体（验证 stream:true）

    void start(const std::string& path,
               std::vector<std::pair<std::string, std::string>> events) {
        io.start();
        srv.router().post(path, [this, events](http::Context& c) {
            seenBodies.push_back(c.request().body);
            auto s = c.sse();
            for (const auto& [ev, data] : events) s->send(data, ev);
            s->close();
        });
        (void)srv.listen("127.0.0.1", 0);
        t = std::thread([&] { loop.loop(); });
        std::this_thread::sleep_for(200ms);
    }
    ~SseMock() {
        loop.stop();
        if (t.joinable()) t.join();
        io.stop();
    }
};
} // namespace

ZTEST(SseTranslate, OpenAiDialect) {
    SseMock m;
    m.start("/v1/chat/completions",
            {{"", R"({"choices":[{"index":0,"delta":{"role":"assistant"}}]})"},
             {"", R"({"choices":[{"index":0,"delta":{"content":"Hel"}}]})"},
             {"", R"({"choices":[{"index":0,"delta":{"content":"lo"}}]})"},
             {"", R"({"choices":[{"index":0,"delta":{"content":"!"},"finish_reason":"stop"}]})"},
             {"", R"({"choices":[],"usage":{"prompt_tokens":3,"completion_tokens":2}})"},
             {"", "[DONE]"}});

    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"o","api":"openai-chat","baseURL":"http://127.0.0.1:)")
        + std::to_string(m.srv.port())
        + R"(/v1","apiKey":"k","models":["m"]}]})").value();
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());

    std::vector<std::string> tokens;
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req,
                         /*onToken=*/[&](std::string_view d) {
                             tokens.emplace_back(d);
                             return true;
                         });
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_EQ(r.content, std::string("Hello!"));
    ZEXPECT_EQ(tokens.size(), size_t{3}); // 逐 token：Hel / lo / !
    ZEXPECT_EQ(tokens[0], std::string("Hel"));
    ZEXPECT_EQ(r.finishReason, std::string("stop"));
    ZEXPECT_EQ(r.usage.promptTokens, 3);
    ZEXPECT_EQ(r.usage.completionTokens, 2);
    // 请求确实带 stream:true
    ZEXPECT_TRUE(m.seenBodies.at(0).find("\"stream\":true") != std::string::npos);
}

ZTEST(SseTranslate, AnthropicDialect) {
    SseMock m;
    m.start("/v1/messages",
            {{"message_start", R"({"type":"message_start","message":{"usage":{"input_tokens":7}}})"},
             {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Wor"}})"},
             {"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"ld"}})"},
             {"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":2}})"},
             {"message_stop", R"({"type":"message_stop"})"}});

    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"a","api":"anthropic","baseURL":"http://127.0.0.1:)")
        + std::to_string(m.srv.port())
        + R"(","apiKey":"k","models":["m"]}]})").value();
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());

    std::vector<std::string> tokens;
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req, [&](std::string_view d) {
        tokens.emplace_back(d);
        return true;
    });
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_EQ(r.content, std::string("World"));
    ZEXPECT_EQ(tokens.size(), size_t{2});
    ZEXPECT_EQ(r.finishReason, std::string("end_turn"));
    ZEXPECT_EQ(r.usage.completionTokens, 2);
}

ZTEST(SseTranslate, GeminiDialect) {
    SseMock m;
    m.start("/v1beta/models/m:streamGenerateContent",
            {{"", R"({"candidates":[{"content":{"parts":[{"text":"Fo"}],"role":"model"},"index":0}]})"},
             {"", R"({"candidates":[{"content":{"parts":[{"text":"od"}],"role":"model"},"index":0}],"usageMetadata":{"promptTokenCount":5}})"},
             {"", R"({"candidates":[{"content":{"parts":[{"text":"!"}],"role":"model"},"finishReason":"STOP","index":0}]})"}});

    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"g","api":"gemini","baseURL":"http://127.0.0.1:)")
        + std::to_string(m.srv.port())
        + R"(/v1beta","apiKey":"k","models":["m"]}]})").value();
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());

    std::vector<std::string> tokens;
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req, [&](std::string_view d) {
        tokens.emplace_back(d);
        return true;
    });
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_EQ(r.content, std::string("Food!"));
    ZEXPECT_EQ(tokens.size(), size_t{3});
    ZEXPECT_EQ(r.finishReason, std::string("STOP"));
}

ZTEST(SseTranslate, NonStreamPathUnchanged) {
    // onToken 为空 → 走整包非流式（stream:false；mock 只回 SSE 流 → 解析失败但零崩溃）
    SseMock m;
    m.start("/v1/chat/completions", {{ "", "[DONE]" }});

    auto cfg = json::JsonValue::parse(
        std::string(R"({"providers":[{"name":"o","api":"openai-chat","baseURL":"http://127.0.0.1:)")
        + std::to_string(m.srv.port())
        + R"(/v1","apiKey":"k","models":["m"]}]})").value();
    ai::ProviderRouter router(ai::RouterConfig::fromJson(cfg).value());
    ai::ChatRequest req;
    req.model = "m";
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    req.messages.push_back(u);
    auto r = router.chat(req); // 无回调 → 非流式
    // mock 只回 SSE 流，非流式解析必然失败——验证的是没有崩溃且走了整包路径
    ZEXPECT_FALSE(r.ok);
    ZEXPECT_TRUE(m.seenBodies.at(0).find("\"stream\":false") != std::string::npos);
}

/* ---------------- M13.4 crew 内嵌仪表盘 ---------------- */

ZTEST(CrewDashboard, ServesSpaHtml) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    crew::mountCrewDashboard(srv.router());
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    // 借 provider_router 的 simpleHttpRequest 做客户端（明文 GET）
    ai::SimpleHttpCall call;
    call.method = "GET";
    call.url = "http://127.0.0.1:" + std::to_string(srv.port()) + "/crew/dashboard";
    auto res = ai::simpleHttpRequest(call);
    ZEXPECT_TRUE(res.ok);
    ZEXPECT_EQ(res.status, 200);
    // 单文件 SPA：引用全部数据端点 + SSE
    ZEXPECT_TRUE(res.body.find("EventSource") != std::string::npos);
    ZEXPECT_TRUE(res.body.find("/crew/tasks") != std::string::npos);
    ZEXPECT_TRUE(res.body.find("approve") != std::string::npos);
    ZEXPECT_TRUE(res.body.find("agents") != std::string::npos);

    loop.stop();
    t.join();
    io.stop();
}

ZTEST_MAIN
