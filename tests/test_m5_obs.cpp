// M5.3 可观测性测试：指标(Prometheus)/Request-Id/status 端到端
#include <algorithm>
#include <chrono>
#include <thread>

#include "ztest.hpp"

#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/metrics/metrics.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- 指标单元 ---------------- */

ZTEST(Metrics, CounterGaugeHistogram) {
    metrics::Counter c;
    c.inc();
    c.inc(41);
    ZEXPECT_EQ(c.value(), int64_t{42});

    metrics::Gauge g;
    g.set(10);
    g.inc(5);
    g.dec(3);
    ZEXPECT_EQ(g.value(), int64_t{12});

    metrics::Histogram h;
    ZEXPECT_EQ(h.count(), int64_t{0});
    ZEXPECT_EQ(h.percentile(0.99), 0.0);
    for (int i = 0; i < 100; ++i) h.observe(100 + i); // 100~199μs → 0.0001 桶
    ZEXPECT_EQ(h.count(), int64_t{100});
    ZEXPECT_EQ(h.sumMicros(), int64_t{100 * 100 + 99 * 100 / 2}); // Σ(100+i)
    ZEXPECT_TRUE(h.percentile(0.5) <= 0.0006); // P50 落 0.0001~0.0005 桶界（粗粒度语义）
    auto lines = h.bucketsForExport("test_lat");
    ZEXPECT_EQ(lines.size(), size_t{16}); // 14 桶 + sum + count
    ZEXPECT_TRUE(lines[0].find("test_lat_bucket{le=\"0.000001\"}") != std::string::npos);
    ZEXPECT_TRUE(lines.back().find("test_lat_count 100") != std::string::npos);
}

ZTEST(Metrics, RegistryExportFormat) {
    auto& reg = metrics::Registry::instance();
    reg.counter("ztest_ctr").inc(7);
    reg.gauge("ztest_gg").set(3);
    const std::string text = reg.exportPrometheus();
    ZEXPECT_TRUE(text.find("# TYPE ztest_ctr counter") != std::string::npos);
    ZEXPECT_TRUE(text.find("ztest_ctr 7") != std::string::npos);
    ZEXPECT_TRUE(text.find("ztest_gg 3") != std::string::npos);
    ZEXPECT_TRUE(text.find("# TYPE") != std::string::npos);
    // 名称可枚举
    const auto names = reg.names();
    ZEXPECT_TRUE(std::find(names.begin(), names.end(), std::string("c:ztest_ctr")) != names.end());
}

/* ---------------- 端到端 ---------------- */

ZTEST(ObsE2E, MetricsRequestIdStatus) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    auto& reg = metrics::Registry::instance();

    server.router().use(http::requestIdMiddleware());
    server.router().use(http::metricsMiddleware(reg));
    server.router().get("/api", [](http::Context& c) { c.text(200, "ok"); });
    server.router().get("/boom", [](http::Context&) { throw std::runtime_error("x"); });
    server.router().get("/metrics", http::prometheusHandler(reg));
    server.router().get("/zenith/status", http::statusHandler());
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(120ms);

    const int64_t before = reg.httpRequests().value();

    // 3 次 /api + 1 次 /boom
    for (int i = 0; i < 3; ++i) {
        auto r = http::HttpClient::fetchBlockingUrl(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/api");
        ZEXPECT_EQ(r.value().status, 200);
        // Request-Id 响应头存在且 32 hex
        const std::string rid = r.value().headers.get("X-Request-Id");
        ZEXPECT_EQ(rid.size(), size_t{32});
    }
    {
        auto r = http::HttpClient::fetchBlockingUrl(
            "http://127.0.0.1:" + std::to_string(server.port()) + "/boom");
        ZEXPECT_EQ(r.value().status, 500);
    }

    // 指标应记录 4 个请求（+2 metrics/status 自身后续查询不计入判断）
    ZEXPECT_TRUE(reg.httpRequests().value() - before >= 4);
    ZEXPECT_TRUE(reg.httpErrors().value() >= 1);
    ZEXPECT_TRUE(reg.httpLatency().count() >= 4);
    ZEXPECT_EQ(reg.httpInflight().value(), int64_t{0}); // 全部完成

    // /metrics 导出合法
    auto m = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/metrics");
    ZEXPECT_EQ(m.value().status, 200);
    ZEXPECT_TRUE(m.value().body.find("zenith_http_requests_total") != std::string::npos);
    ZEXPECT_TRUE(m.value().body.find("zenith_http_request_latency_us_bucket") !=
                 std::string::npos);
    ZEXPECT_TRUE(m.value().body.find("# TYPE zenith_http_requests_total counter") !=
                 std::string::npos);

    // /zenith/status JSON
    auto st = http::HttpClient::fetchBlockingUrl(
        "http://127.0.0.1:" + std::to_string(server.port()) + "/zenith/status");
    ZEXPECT_EQ(st.value().status, 200);
    auto v = http::json::JsonValue::parse(st.value().body);
    ZEXPECT_TRUE(static_cast<bool>(v));
    ZEXPECT_TRUE(v.value()["http_requests_total"].asInt() >= 4);
    ZEXPECT_TRUE(v.value()["uptime_ms"].asInt() >= 0);
    ZEXPECT_TRUE(v.value()["latency"]["count"].asInt() >= 4);

    // 透传：外部 Request-Id 应原样回写
    {
        http::ClientRequest rq;
        rq.url = "http://127.0.0.1:" + std::to_string(server.port()) + "/api";
        rq.headers.set("X-Request-Id", "my-trace-id-123");
        auto r = http::HttpClient::fetchBlocking(rq);
        ZEXPECT_EQ(r.value().headers.get("X-Request-Id"), std::string("my-trace-id-123"));
    }

    loop.stop();
    t.join();
    io.stop();
}

ZTEST_MAIN
