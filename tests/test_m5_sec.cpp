// M5.2 安全测试：SHA-256/HMAC(NIST 向量+OpenSSL 交叉) / JWT / CORS / 限流 / 安全头 / 登录链路
#include <algorithm>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include "ztest.hpp"

#include <openssl/hmac.h>
#include <openssl/sha.h>

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/jwt.hpp"
#include "zenith/http/security.hpp"

using namespace zenith;
using namespace zenith::http;
using namespace std::chrono_literals;

/* ---------------- SHA-256 / HMAC ---------------- */

ZTEST(Sha256, NistVectors) {
    // FIPS 180-4 标准向量
    ZEXPECT_EQ(crypto::Sha256::hex(""),
               std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    ZEXPECT_EQ(crypto::Sha256::hex("abc"),
               std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    ZEXPECT_EQ(crypto::Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
               std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    // 多轮 update 等价（final 后手工转 hex）
    {
        crypto::Sha256 h;
        h.update("ab");
        h.update("c");
        uint8_t d[32];
        h.final(d);
        static const char* kHex = "0123456789abcdef";
        std::string hex;
        for (uint8_t b : std::vector<uint8_t>(d, d + 32)) {
            hex.push_back(kHex[b >> 4]);
            hex.push_back(kHex[b & 0xF]);
        }
        ZEXPECT_EQ(hex,
                   std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
}

ZTEST(Hmac, RfcVectorsAndOpenSSLCross) {
    uint8_t out[32];
    // RFC 4231 Test Case 2：key="Jefe", data="what do ya want for nothing?"
    crypto::hmacSha256("Jefe", "what do ya want for nothing?", out);
    crypto::Sha256 formatter; // 仅用 hex 逻辑 → 手工转
    static const char* kH = "0123456789abcdef";
    std::string hex;
    for (uint8_t b : std::vector<uint8_t>(out, out + 32)) {
        hex.push_back(kH[b >> 4]);
        hex.push_back(kH[b & 0xF]);
    }
    ZEXPECT_EQ(hex, std::string("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"));

    // OpenSSL 交叉：2000 组随机样本
    std::mt19937 rng{std::random_device{}()};
    int mismatch = 0;
    for (int i = 0; i < 2000; ++i) {
        std::string key, msg;
        const size_t kl = rng() % 128, ml = rng() % 512;
        for (size_t j = 0; j < kl; ++j) key.push_back(static_cast<char>(rng() & 0xFF));
        for (size_t j = 0; j < ml; ++j) msg.push_back(static_cast<char>(rng() & 0xFF));

        uint8_t mine[32], ref[32];
        crypto::hmacSha256(key, msg, mine);
        unsigned int refLen = 0;
        HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(msg.data()), msg.size(), ref, &refLen);
        if (!crypto::constantTimeEquals(mine, ref, 32)) ++mismatch;
    }
    ZEXPECT_EQ(mismatch, 0);
}

/* ---------------- JWT ---------------- */

ZTEST(Jwt, SignVerifyExpiry) {
    const std::string secret = "zenith-secret-key";
    const int64_t now = 1760000000;

    auto tok = jwt::sign(secret, R"({"sub":"user-1","role":"admin"})", 3600, now);
    ZEXPECT_TRUE(tok.find('.') != std::string::npos);
    // 两点分隔三段
    ZEXPECT_EQ(static_cast<int>(std::count(tok.begin(), tok.end(), '.')), 2);

    auto v = jwt::verify(secret, tok, now + 100);
    ZEXPECT_TRUE(v.ok);
    ZEXPECT_TRUE(v.payload.find("\"sub\":\"user-1\"") != std::string::npos);
    auto exp = jwt::claimInt(v.payload, "exp");
    ZEXPECT_TRUE(exp.has_value());
    ZEXPECT_EQ(*exp, now + 3600);

    // 过期（超出 leeway）
    auto v2 = jwt::verify(secret, tok, now + 3600 + 31);
    ZEXPECT_FALSE(v2.ok);
    ZEXPECT_EQ(v2.error, std::string("token expired"));

    // 错误密钥
    auto v3 = jwt::verify("wrong", tok, now);
    ZEXPECT_FALSE(v3.ok);
    ZEXPECT_EQ(v3.error, std::string("bad signature"));

    // 篡改 payload
    std::string tampered = tok;
    tampered[tampered.find('.', 0) + 1] ^= 0x01;
    auto v4 = jwt::verify(secret, tampered, now);
    ZEXPECT_FALSE(v4.ok);

    // 畸形
    auto v5 = jwt::verify(secret, "not-a-jwt", now);
    ZEXPECT_FALSE(v5.ok);
}

/* ---------------- 端到端：登录 → JWT → 保护路由 + 限流 + CORS + 安全头 ---------------- */

ZTEST(SecurityE2E, FullChain) {
    const std::string secret = "e2e-secret";
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    auto& r = server.router();

    r.use(http::securityHeadersMiddleware());
    r.use(http::corsMiddleware(http::CorsOptions{.allowOrigin = "https://app.example.com,https://web.example.com",
                                                 .allowCredentials = true}));
    // 限流只挂在 /login
    auto limiter = std::make_shared<http::TokenBucketLimiter>(2.0, 3.0); // 2/s，桶 3
    r.post("/login", http::Handler{[](http::Context& c) {
               const auto body = c.bodyJson();
               if (body["user"].asString() == "tom" && body["pass"].asString() == "pw") {
                   http::json::JsonValue v = http::json::JsonValue::object();
                   v["token"] = http::jwt::sign("e2e-secret", R"({"sub":"tom"})", 600,
                                                std::chrono::duration_cast<std::chrono::seconds>(
                                                    std::chrono::system_clock::now()
                                                        .time_since_epoch())
                                                    .count());
                   c.json(200, v);
               } else {
                   c.status(401).text(401, "bad credentials");
               }
           }},
           {http::rateLimitMiddleware(http::RateLimitOptions{.ratePerSec = 2.0, .burst = 3.0},
                                      limiter)});
    r.use(http::jwtAuthMiddleware(http::JwtAuthOptions{secret, .exemptPrefixes = "/login"}));
    r.options("/protected", [](http::Context&) { /* 占位：cors 中间件在链中先行短路 */ });
    r.get("/protected", [](http::Context& c) {
        c.text(200, "hello " +
                        (c.contextAs<std::string>() != nullptr
                             ? http::json::JsonValue::parse(*c.contextAs<std::string>())
                                   .value()["sub"]
                                   .asString()
                             : std::string("?")));
    });

    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);
    const auto url = [&](const std::string& p) {
        return "http://127.0.0.1:" + std::to_string(server.port()) + p;
    };

    // 1) 登录（正确凭据 → 拿 token）
    http::ClientRequest login;
    login.url = url("/login");
    login.method = http::HttpMethod::Post;
    login.body = R"({"user":"tom","pass":"pw"})";
    auto lr = http::HttpClient::fetchBlocking(login);
    ZEXPECT_EQ(lr.value().status, 200);
    auto token = http::json::JsonValue::parse(lr.value().body).value()["token"].asString();
    ZEXPECT_FALSE(token.empty());
    // 安全头 + CORS 白名单回显
    ZEXPECT_EQ(lr.value().headers.get("X-Content-Type-Options"), std::string("nosniff"));
    ZEXPECT_EQ(lr.value().headers.get("X-Frame-Options"), std::string("DENY"));

    // 2) 错误凭据
    login.body = R"({"user":"tom","pass":"wrong"})";
    auto bad = http::HttpClient::fetchBlocking(login);
    ZEXPECT_EQ(bad.value().status, 401);

    // 3) 受保护路由：无 token / 带 token
    auto anon = http::HttpClient::fetchBlockingUrl(url("/protected"));
    ZEXPECT_EQ(anon.value().status, 401);
    ZEXPECT_TRUE(anon.value().headers.get("WWW-Authenticate").find("Bearer") !=
                 std::string::npos);
    http::ClientRequest auth;
    auth.url = url("/protected");
    auth.headers.set("Authorization", "Bearer " + token);
    auto ok = http::HttpClient::fetchBlocking(auth);
    ZEXPECT_EQ(ok.value().status, 200);
    ZEXPECT_EQ(ok.value().body, std::string("hello tom"));

    // 4) 限流：桶 3 —— 连打 6 次登录，后段 429
    int saw429 = 0, saw200 = 0;
    for (int i = 0; i < 6; ++i) {
        login.body = R"({"user":"x","pass":"y"})";
        auto rl = http::HttpClient::fetchBlocking(login);
        if (rl.value().status == 429) {
            ++saw429;
            ZEXPECT_FALSE(rl.value().headers.get("Retry-After").empty());
        } else if (rl.value().status == 401) {
            ++saw200; // 正常拒绝（未超限）
        }
    }
    ZEXPECT_TRUE(saw429 >= 1);
    ZEXPECT_TRUE(saw200 + saw429 == 6);

    // 5) CORS：白名单 origin 回显 / 非白名单 403 / 预检 204
    {
        http::ClientRequest rq;
        rq.url = url("/protected");
        rq.headers.set("Authorization", "Bearer " + token);
        rq.headers.set("Origin", "https://app.example.com");
        auto cr = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(cr.value().headers.get("Access-Control-Allow-Origin"),
                   std::string("https://app.example.com"));
    }
    {
        http::ClientRequest rq;
        rq.url = url("/protected");
        rq.headers.set("Origin", "https://evil.example.net");
        auto cr = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(cr.value().status, 403);
    }
    {
        http::ClientRequest rq;
        rq.url = url("/protected");
        rq.method = http::HttpMethod::Options;
        rq.headers.set("Origin", "https://web.example.com");
        rq.headers.set("Access-Control-Request-Method", "GET");
        auto pf = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(pf.value().status, 204);
        ZEXPECT_FALSE(pf.value().headers.get("Access-Control-Allow-Methods").empty());
    }

    loop.stop();
    t.join();
    io.stop();
}

ZTEST(TokenBucket, BurstAndRate) {
    http::TokenBucketLimiter lim(10.0, 5.0); // 10/s，桶 5
    int ok = 0;
    for (int i = 0; i < 10; ++i) {
        if (lim.allow("k")) ++ok;
    }
    ZEXPECT_EQ(ok, 5); // 突发 5 枚，随后即时空窗
    ZEXPECT_EQ(lim.trackedKeys(), size_t{1});
    // 不同键独立
    ZEXPECT_TRUE(lim.allow("other"));
}

ZTEST_MAIN
