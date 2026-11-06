// M8.7 W3C Trace Context：解析/序列化/校验 + 跨服务两跳传播
#include <string>
#include <thread>

#include "ztest.hpp"

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/obs/trace_context.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
const char* kValid = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
}  // namespace

ZTEST(TraceContext, ParseValid) {
    auto tc = obs::TraceContext::parse(kValid);
    ZEXPECT_TRUE(tc.has_value());
    if (!tc.has_value()) return;
    ZEXPECT_EQ(tc->traceId, std::string("4bf92f3577b34da6a3ce929d0e0e4736"));
    ZEXPECT_EQ(tc->spanId, std::string("00f067aa0ba902b7"));
    ZEXPECT_TRUE(tc->sampled);
    ZEXPECT_TRUE(tc->valid());
    ZEXPECT_EQ(tc->traceparent(), std::string(kValid));  // 往返一致
}

ZTEST(TraceContext, ParseRejectsInvalid) {
    // 太短
    ZEXPECT_FALSE(obs::TraceContext::parse("00-abc-def-01").has_value());
    // 大写十六进制（规范要求小写）
    ZEXPECT_FALSE(
        obs::TraceContext::parse("00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01")
            .has_value());
    // trace-id 全零
    ZEXPECT_FALSE(
        obs::TraceContext::parse("00-00000000000000000000000000000000-00f067aa0ba902b7-01")
            .has_value());
    // parent-id 全零
    ZEXPECT_FALSE(
        obs::TraceContext::parse("00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01")
            .has_value());
    // 版本 ff 保留
    ZEXPECT_FALSE(
        obs::TraceContext::parse("ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01")
            .has_value());
    // 版本 00 带多余尾巴
    ZEXPECT_FALSE(
        obs::TraceContext::parse("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra")
            .has_value());
    // 分隔符错位
    ZEXPECT_FALSE(
        obs::TraceContext::parse("00_4bf92f3577b34da6a3ce929d0e0e4736_00f067aa0ba902b7_01")
            .has_value());
    // 空
    ZEXPECT_FALSE(obs::TraceContext::parse("").has_value());
}

ZTEST(TraceContext, ChildKeepsTraceChangesSpan) {
    auto tc = obs::TraceContext::parse(kValid).value();
    const auto child = tc.child();
    ZEXPECT_EQ(child.traceId, tc.traceId);
    ZEXPECT_NE(child.spanId, tc.spanId);
    ZEXPECT_TRUE(child.valid());
}

ZTEST(TraceContext, GeneratedIsValidAndUnique) {
    const auto a = obs::TraceContext::generate();
    const auto b = obs::TraceContext::generate();
    ZEXPECT_TRUE(a.valid());
    ZEXPECT_TRUE(b.valid());
    ZEXPECT_NE(a.traceId, b.traceId);
    ZEXPECT_TRUE(a.traceparent().size() == 55);
}

ZTEST(TraceContext, ScopeRestoresPrevious) {
    ZEXPECT_FALSE(obs::currentTrace().has_value());
    const auto tc = obs::TraceContext::parse(kValid).value();
    {
        const obs::TraceScope inner(tc);
        // 注意：ZEXPECT_EQ 用 auto&& 绑定实参，直接写 currentTrace()->traceId 会绑到
        // 已销毁的临时 optional 成员上（悬垂），必须先落成本地变量。
        const auto cur = obs::currentTrace();
        ZEXPECT_TRUE(cur.has_value());
        ZEXPECT_EQ(cur->traceId, tc.traceId);
        {
            const obs::TraceScope nested(std::nullopt);
            ZEXPECT_FALSE(obs::currentTrace().has_value());  // 内层清空
        }
        ZEXPECT_TRUE(obs::currentTrace().has_value());  // 恢复外层
    }
    ZEXPECT_FALSE(obs::currentTrace().has_value());  // 整体复原
}

/* ---------------- 两跳传播：A 收到 → A 的下游调用带上同一条 trace ---------------- */

namespace {
struct TraceFixture {
    net::EventLoop loopA;
    net::EventLoop loopB;
    net::EventLoopThreadPool ioA{2};
    net::EventLoopThreadPool ioB{1};
    http::HttpServer a{&loopA, &ioA};
    http::HttpServer b{&loopB, &ioB};
    std::thread ta;
    std::thread tb;
    std::string bUrl;

    TraceFixture() {
        ioA.start();
        ioB.start();
        // B：把收到的 traceparent 原样回显
        b.router().get("/echo", [](http::Context& c) { c.text(200, c.header("traceparent")); });
        (void)b.listen("127.0.0.1", 0);
        bUrl = "http://127.0.0.1:" + std::to_string(b.port()) + "/echo";

        // A：用 HttpClient 调 B，把 B 看到的 traceparent 返回给调用方
        a.router().get("/hop", [this](http::Context& c) {
            auto r = http::HttpClient::fetchBlockingUrl(bUrl);
            c.text(200, r ? r.value().body : std::string("<downstream failed>"));
        });
        // A：回显本请求的线程上下文（服务端提取结果）
        a.router().get("/whoami", [](http::Context& c) {
            const auto tc = obs::currentTrace();
            c.text(200, tc.has_value() ? tc->traceparent() : std::string("none"));
        });
        (void)a.listen("127.0.0.1", 0);
        ta = std::thread([this] { loopA.loop(); });
        tb = std::thread([this] { loopB.loop(); });
        std::this_thread::sleep_for(150ms);
    }
    ~TraceFixture() {
        loopA.stop();
        loopB.stop();
        ta.join();
        tb.join();
        ioA.stop();
        ioB.stop();
    }
    std::string aUrl(const std::string& path) const {
        return "http://127.0.0.1:" + std::to_string(a.port()) + path;
    }
};
}  // namespace

ZTEST(TraceE2E, ServerExtractsInboundHeader) {
    TraceFixture f;
    http::ClientRequest rq;
    rq.url = f.aUrl("/whoami");
    rq.headers.set("traceparent", kValid);
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_EQ(r.value().body, std::string(kValid));  // 服务端解析出同一条链路
}

ZTEST(TraceE2E, ServerGeneratesWhenHeaderAbsent) {
    TraceFixture f;
    auto r = http::HttpClient::fetchBlockingUrl(f.aUrl("/whoami"));
    ZEXPECT_TRUE(static_cast<bool>(r));
    ZEXPECT_TRUE(r.value().body.size() == 55);
    ZEXPECT_TRUE(r.value().body.rfind("00-", 0) == 0);
}

ZTEST(TraceE2E, DownstreamCallCarriesSameTrace) {
    TraceFixture f;
    const auto tc = obs::TraceContext::parse(kValid).value();
    http::ClientRequest rq;
    rq.url = f.aUrl("/hop");
    rq.headers.set("traceparent", tc.traceparent());
    auto r = http::HttpClient::fetchBlocking(rq);
    ZEXPECT_TRUE(static_cast<bool>(r));
    // B 收到的 traceparent 必须与 A 收到的一致 —— 说明提取+注入链路通了
    auto seen = obs::TraceContext::parse(r.value().body);
    ZEXPECT_TRUE(seen.has_value());
    if (seen.has_value()) {
        ZEXPECT_EQ(seen->traceId, tc.traceId);
    }
}

ZTEST_MAIN
