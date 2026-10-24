// M13 第四批测试：sched_cron / obs_otel / auth_oauth / net_mqtt
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "ztest.hpp"

#include "zenith/concurrency/cron.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/oauth.hpp"
#include "zenith/http/otel.hpp"
#include "zenith/net/mqtt.hpp"

using namespace zenith;
using namespace std::chrono_literals;

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

/* ---------------- sched_cron ---------------- */

ZTEST(Cron, ParseAndNext) {
    // 非法表达式
    ZEXPECT_FALSE(static_cast<bool>(concurrency::CronExpr::parse("* * * *")));
    ZEXPECT_FALSE(static_cast<bool>(concurrency::CronExpr::parse("60 * * * *")));
    ZEXPECT_FALSE(static_cast<bool>(concurrency::CronExpr::parse("* 24 * * *")));
    ZEXPECT_FALSE(static_cast<bool>(concurrency::CronExpr::parse("a * * * *")));

    const int64_t t = 1792854493; // 固定 epoch（2026-10-24 前后）
    auto everyMin = concurrency::CronExpr::parse("* * * * *");
    ZEXPECT_TRUE(static_cast<bool>(everyMin));
    const int64_t n1 = everyMin.value().nextAfter(t);
    ZEXPECT_TRUE(n1 > t);
    ZEXPECT_EQ(n1, t - t % 60 + 60); // 下一分钟整（时区无关：分钟粒度对齐）

    auto every5 = concurrency::CronExpr::parse("*/5 * * * *");
    ZEXPECT_TRUE(static_cast<bool>(every5));
    const int64_t n5 = every5.value().nextAfter(t);
    ZEXPECT_TRUE(n5 > t && n5 <= t + 300);
    ZEXPECT_EQ(n5 % 300, int64_t{0});

    // 工作日 9 点：下次必在 7 天内且严格大于 t（跨时区稳健断言）
    auto weekday = concurrency::CronExpr::parse("0 9 * * 1-5");
    ZEXPECT_TRUE(static_cast<bool>(weekday));
    const int64_t nw = weekday.value().nextAfter(t);
    ZEXPECT_TRUE(nw > t);
    ZEXPECT_TRUE(nw - t <= 7 * 86400);
}

ZTEST(Cron, SchedulerFiresAndRemoves) {
    concurrency::CronScheduler sched;
    std::atomic<int> fired{0};
    const uint64_t id = sched.add("* * * * *", [&](int64_t) { ++fired; });
    ZEXPECT_TRUE(id != 0);
    ZEXPECT_EQ(sched.jobCount(), size_t{1});
    ZEXPECT_FALSE(sched.remove(9999)); // 不存在
    ZEXPECT_TRUE(sched.remove(id));
    ZEXPECT_EQ(sched.jobCount(), size_t{0});
    ZEXPECT_EQ(sched.add("bad expr", {}), uint64_t{0}); // 解析失败 0

    const uint64_t id2 = sched.add("* * * * *", [&](int64_t) { ++fired; });
    (void)id2;
    // 等下一次分钟边界触发（最长 ~70s）
    for (int i = 0; i < 140 && fired.load() == 0; ++i) {
        std::this_thread::sleep_for(500ms);
    }
    ZEXPECT_TRUE(fired.load() >= 1);
    ZEXPECT_TRUE(sched.triggerCount() >= 1);
    sched.stop();
}

/* ---------------- obs_otel ---------------- */

ZTEST(Otel, ExportSpansToCollector) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    std::string lastBody;
    int posts = 0;
    srv.router().post("/v1/traces", [&](http::Context& c) {
        lastBody = c.request().body;
        ++posts;
        c.json(200, json::JsonValue());
    });
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    obs::OtelConfig cfg;
    cfg.endpoint = "http://127.0.0.1:" + std::to_string(srv.port()) + "/v1/traces";
    cfg.serviceName = "zenith-test";
    cfg.flushIntervalMs = 0; // 手动 flush
    obs::OtelExporter ex(cfg);

    auto parent = ex.beginSpan("request");
    auto child = ex.beginSpan("db.query", parent.traceId, parent.spanId);
    ex.endSpan(child, {{"db", "sqlite"}});
    ex.endSpan(parent, {{"route", "/api/x"}});
    ZEXPECT_EQ(ex.pending(), size_t{2});

    const size_t sent = ex.flushNow();
    ZEXPECT_EQ(sent, size_t{2});
    ZEXPECT_EQ(ex.exportedSpans(), uint64_t{2});
    ZEXPECT_EQ(ex.failedFlushes(), uint64_t{0});
    ZEXPECT_EQ(posts, 1);
    ZEXPECT_TRUE(lastBody.find("zenith-test") != std::string::npos);
    ZEXPECT_TRUE(lastBody.find("db.query") != std::string::npos);
    ZEXPECT_TRUE(lastBody.find("parentSpanId") != std::string::npos);

    // traceId 32 hex / spanId 16 hex
    auto v = json::JsonValue::parse(lastBody);
    const auto& span0 = v.value()["resourceSpans"].at(size_t{0})["scopeSpans"]
                             .at(size_t{0})["spans"].at(size_t{0});
    ZEXPECT_EQ(span0["traceId"].asString().size(), size_t{32});
    ZEXPECT_EQ(span0["spanId"].asString().size(), size_t{16});

    loop.stop();
    t.join();
    io.stop();
}

ZTEST(Otel, FailedFlushKeepsQueue) {
    // 端点不可达：失败回队不丢数据
    obs::OtelConfig cfg;
    cfg.endpoint = "http://127.0.0.1:1/v1/traces"; // 无人监听
    cfg.flushIntervalMs = 0;
    obs::OtelExporter ex(cfg);
    auto s = ex.beginSpan("x");
    ex.endSpan(s);
    ZEXPECT_EQ(ex.flushNow(), size_t{0});
    ZEXPECT_EQ(ex.failedFlushes(), uint64_t{1});
    ZEXPECT_EQ(ex.pending(), size_t{1}); // 回队保留
}

/* ---------------- auth_oauth ---------------- */

ZTEST(OAuth, PkceAndAuthorizeUrl) {
    auto pk = http::oauth::makePkce();
    ZEXPECT_TRUE(pk.valid());           // verifier>=43, challenge 43
    ZEXPECT_EQ(pk.challenge.size(), size_t{43});
    ZEXPECT_TRUE(pk.challenge.find('+') == std::string::npos); // base64url
    ZEXPECT_TRUE(pk.challenge.find('/') == std::string::npos);
    auto pk2 = http::oauth::makePkce();
    ZEXPECT_TRUE(pk2.verifier != pk.verifier); // 随机性

    http::oauth::OAuthClientConfig cfg;
    cfg.authorizeUrl = "https://idp.example.com/authorize";
    cfg.tokenUrl = "http://127.0.0.1:1/token";
    cfg.clientId = "cid 1"; // 含空格验证 URL 编码
    cfg.redirectUri = "http://localhost:8093/callback";
    cfg.scope = "read write";
    const std::string state = http::oauth::randomState();
    ZEXPECT_EQ(state.size(), size_t{64});
    const std::string url = http::oauth::buildAuthorizeUrl(cfg, state, pk);
    ZEXPECT_TRUE(url.find("response_type=code") != std::string::npos);
    ZEXPECT_TRUE(url.find("client_id=cid%201") != std::string::npos);
    ZEXPECT_TRUE(url.find("state=" + state) != std::string::npos);
    ZEXPECT_TRUE(url.find("code_challenge=" + pk.challenge) != std::string::npos);
    ZEXPECT_TRUE(url.find("code_challenge_method=S256") != std::string::npos);
    ZEXPECT_TRUE(url.find("scope=read%20write") != std::string::npos);
}

ZTEST(OAuth, ExchangeCodeWithMockIdp) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    std::string lastForm;
    srv.router().post("/token", [&](http::Context& c) {
        lastForm = c.request().body;
        json::JsonValue j = json::JsonValue::object();
        j["access_token"] = "at-123";
        j["refresh_token"] = "rt-456";
        j["token_type"] = "Bearer";
        j["expires_in"] = int64_t{3600};
        c.json(200, j);
    });
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    http::oauth::OAuthClientConfig cfg;
    cfg.authorizeUrl = "http://127.0.0.1:" + std::to_string(srv.port()) + "/authorize";
    cfg.tokenUrl = "http://127.0.0.1:" + std::to_string(srv.port()) + "/token";
    cfg.clientId = "cid";
    cfg.clientSecret = "secret";
    cfg.redirectUri = "http://localhost/cb";
    auto pk = http::oauth::makePkce();

    auto tok = http::oauth::exchangeCode(cfg, "auth-code-9", pk);
    ZEXPECT_TRUE(tok.ok);
    ZEXPECT_EQ(tok.accessToken, std::string("at-123"));
    ZEXPECT_EQ(tok.refreshToken, std::string("rt-456"));
    ZEXPECT_EQ(tok.expiresInSeconds, int64_t{3600});
    ZEXPECT_TRUE(lastForm.find("grant_type=authorization_code") != std::string::npos);
    ZEXPECT_TRUE(lastForm.find("code=auth-code-9") != std::string::npos);
    ZEXPECT_TRUE(lastForm.find("code_verifier=" + pk.verifier) != std::string::npos);
    ZEXPECT_TRUE(lastForm.find("client_secret=secret") != std::string::npos);

    auto tok2 = http::oauth::refresh(cfg, "rt-456");
    ZEXPECT_TRUE(tok2.ok);
    ZEXPECT_TRUE(lastForm.find("grant_type=refresh_token") != std::string::npos);

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- net_mqtt ---------------- */

namespace {
/// 测试用 mini broker：原始阻塞 socket，按脚本应答
class MiniBroker {
public:
    bool start() {
#if defined(_WIN32)
        static std::once_flag wsa;
        std::call_once(wsa, [] {
            WSADATA d;
            WSAStartup(MAKEWORD(2, 2), &d);
        });
#endif
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        if (::listen(fd_, 1) != 0) return false;
        sockaddr_in got{};
        socklen_t len = sizeof(got);
        if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&got), &len) != 0) return false;
        port_ = ntohs(got.sin_port);
        return true;
    }
    [[nodiscard]] uint16_t port() const { return port_; }

    /// 运行脚本：CONNECT→CONNACK；SUBSCRIBE→SUBACK；PUBLISH(up)记录并下行回推；
    /// PINGREQ→PINGRESP；读到 DISCONNECT 结束
    void run(std::string downTopic, std::string downPayload, std::string* gotUpTopic,
             std::string* gotUpPayload) {
        timeval tv{5, 0};
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char*>(&tv), sizeof(tv));
        client_ = ::accept(fd_, nullptr, nullptr);
        ::closesocket(fd_);
        fd_ = -1;
        if (client_ < 0) return;
        for (;;) {
            uint8_t h = 0;
            auto body = readPacket(h);
            if (!body) break;
            const uint8_t type = h >> 4;
            if (type == 1) { // CONNECT → CONNACK(session present=0, code=0)
                writeRaw(std::string("\x20\x02\x00\x00", 4));
            } else if (type == 8) { // SUBSCRIBE → SUBACK
                const uint16_t pid = static_cast<uint16_t>(
                    (static_cast<uint8_t>(body.value()[0]) << 8) |
                    static_cast<uint8_t>(body.value()[1]));
                std::string ack;
                ack.push_back(static_cast<char>(0x90));
                ack.push_back('\x03');
                ack.push_back(static_cast<char>((pid >> 8) & 0xFF));
                ack.push_back(static_cast<char>(pid & 0xFF));
                ack.push_back('\x00'); // granted qos0
                writeRaw(ack);
            } else if (type == 3) { // PUBLISH 上行 → 回推下行
                const uint16_t tlen = static_cast<uint16_t>(
                    (static_cast<uint8_t>(body.value()[0]) << 8) |
                    static_cast<uint8_t>(body.value()[1]));
                if (gotUpTopic) *gotUpTopic = body.value().substr(2, tlen);
                if (gotUpPayload) *gotUpPayload = body.value().substr(2 + tlen);
                // 下行 QoS0 PUBLISH
                std::string pkt;
                pkt.push_back(static_cast<char>(0x30));
                const size_t remain = 2 + downTopic.size() + downPayload.size();
                pkt.push_back(static_cast<char>(remain)); // 短包单字节长度
                pkt.push_back(static_cast<char>((downTopic.size() >> 8) & 0xFF));
                pkt.push_back(static_cast<char>(downTopic.size() & 0xFF));
                pkt += downTopic + downPayload;
                writeRaw(pkt);
            } else if (type == 12) { // PINGREQ → PINGRESP
                writeRaw(std::string("\xD0\x00", 2));
            } else if (type == 14) { // DISCONNECT
                break;
            }
        }
        if (client_ >= 0) {
#if defined(_WIN32)
            ::closesocket(client_);
#else
            ::close(client_);
#endif
            client_ = -1;
        }
    }

private:
    bool readN(char* buf, size_t n) {
        size_t got = 0;
        while (got < n) {
            const int r = ::recv(client_, buf + got, static_cast<int>(n - got), 0);
            if (r <= 0) return false;
            got += static_cast<size_t>(r);
        }
        return true;
    }
    core::Result<std::string> readPacket(uint8_t& headerByte) {
        char b = 0;
        if (!readN(&b, 1)) {
            return core::Result<std::string>::Err(core::Error(600, "eof"));
        }
        headerByte = static_cast<uint8_t>(b);
        size_t remain = 0;
        int shift = 0;
        for (;;) {
            if (!readN(&b, 1)) {
                return core::Result<std::string>::Err(core::Error(600, "eof"));
            }
            const uint8_t x = static_cast<uint8_t>(b);
            remain |= static_cast<size_t>(x & 0x7F) << shift;
            if ((x & 0x80) == 0) break;
            shift += 7;
        }
        std::string body(remain, '\0');
        if (remain > 0 && !readN(body.data(), remain)) {
            return core::Result<std::string>::Err(core::Error(600, "eof"));
        }
        return core::Result<std::string>::Ok(std::move(body));
    }
    bool writeRaw(const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            const int n = ::send(client_, s.data() + sent,
                                 static_cast<int>(s.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    int fd_ = -1;
    int client_ = -1;
    uint16_t port_ = 0;
};
} // namespace

ZTEST(Mqtt, ConnectPubSubPingAgainstMiniBroker) {
    MiniBroker br;
    ZEXPECT_TRUE(br.start());
    std::string upTopic, upPayload;
    std::thread brokerThread([&] {
        br.run("down/topic", "from-broker", &upTopic, &upPayload);
    });
    std::this_thread::sleep_for(100ms);

    auto c = net::mqtt::MqttClient::connect("127.0.0.1", br.port(), "zenith-test");
    ZEXPECT_TRUE(static_cast<bool>(c));

    ZEXPECT_TRUE(static_cast<bool>(c.value().subscribe("up/topic", 0)));
    ZEXPECT_TRUE(static_cast<bool>(c.value().publish("up/topic", "hello-mqtt", 0)));

    std::atomic<bool> got{false};
    std::string gotTopic, gotPayload;
    c.value().onMessage([&](const net::mqtt::MqttMessage& m) {
        gotTopic = m.topic;
        gotPayload = m.payload;
        got.store(true);
    });
    for (int i = 0; i < 50 && !got.load(); ++i) {
        c.value().pollOnce(200);
    }
    ZEXPECT_TRUE(got.load());
    ZEXPECT_EQ(gotTopic, std::string("down/topic"));
    ZEXPECT_EQ(gotPayload, std::string("from-broker"));

    ZEXPECT_TRUE(static_cast<bool>(c.value().pingreq()));
    c.value().disconnect();
    brokerThread.join();
    ZEXPECT_EQ(upTopic, std::string("up/topic"));
    ZEXPECT_EQ(upPayload, std::string("hello-mqtt"));
}

ZTEST(Mqtt, RefusedConnection) {
    // 端口 1（无监听）→ 连接失败
    auto c = net::mqtt::MqttClient::connect("127.0.0.1", 1, "x");
    ZEXPECT_FALSE(static_cast<bool>(c));
}

ZTEST_MAIN
