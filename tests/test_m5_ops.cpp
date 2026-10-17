// M5.6 测试：配置热重载 + 管理台 REST 端到端 + WorkerSupervisor(POSIX 门控)
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/config/hot_reload.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

#ifndef ZENITH_NO_DB
  #include "zenith/db/db_service.hpp"
#endif

#if defined(ZENITH_PLATFORM_LINUX) || defined(ZENITH_PLATFORM_MACOS)
  #define ZTEST_HAS_POSIX 1
#endif
#include "zenith/net/worker_supervisor.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- 配置热重载 ---------------- */

ZTEST(HotReload, FileChangeReloadsSnapshot) {
    const std::string path = "zhot.tmp.json";
    { std::ofstream(path, std::ios::binary) << R"({"app":{"mode":"dev","level":1}})"; }

    config::HotReloader hr(path, 100);
    std::atomic<int> changes{0};
    std::atomic<int> errors{0};
    hr.onChange([&](config::HotReloader::ConfigPtr) { changes.fetch_add(1); });
    hr.onError([&](const std::string&) { errors.fetch_add(1); });

    hr.start();
    ZEXPECT_TRUE(hr.loaded());
    ZEXPECT_EQ(hr.get()->get("app.mode"), std::string("dev"));

    // 触碰 mtime + 内容变更
    std::this_thread::sleep_for(300ms);
    { std::ofstream(path, std::ios::binary) << R"({"app":{"mode":"prod","level":2}})"; }

    // 首次装载即触发一次 onChange —— 等待“变更后”的第二次
    const int baseline = changes.load();
    for (int i = 0; i < 80 && changes.load() <= baseline; ++i) {
        std::this_thread::sleep_for(50ms);
    }
    ZEXPECT_TRUE(changes.load() >= baseline + 1);
    ZEXPECT_TRUE(hr.reloadCount() >= 2);
    ZEXPECT_EQ(hr.get()->get("app.mode"), std::string("prod"));
    ZEXPECT_EQ(hr.get()->get("app.level"), std::string("2"));

    // 坏 JSON → 保留旧快照 + 错误回调
    std::this_thread::sleep_for(300ms);
    { std::ofstream(path, std::ios::binary) << "{broken"; }
    for (int i = 0; i < 60 && errors.load() == 0; ++i) std::this_thread::sleep_for(50ms);
    ZEXPECT_TRUE(errors.load() >= 1);
    ZEXPECT_EQ(hr.get()->get("app.mode"), std::string("prod")); // 旧值仍在

    // 手动 reload 恢复
    std::this_thread::sleep_for(200ms);
    { std::ofstream(path, std::ios::binary) << R"({"app":{"mode":"fixed"}})"; }
    ZEXPECT_TRUE(hr.reload());
    ZEXPECT_EQ(hr.get()->get("app.mode"), std::string("fixed"));

    hr.stop();
    std::filesystem::remove(path);
}

ZTEST(HotReload, ManualOnlyMode) {
    const std::string path = "zhot2.tmp.json";
    { std::ofstream(path, std::ios::binary) << R"({"v":1})"; }
    config::HotReloader hr(path, 0); // 无轮询
    hr.start();
    ZEXPECT_TRUE(hr.loaded());
    { std::ofstream(path, std::ios::binary) << R"({"v":2})"; }
    std::this_thread::sleep_for(300ms);
    ZEXPECT_EQ(hr.get()->get("v"), std::string("1")); // 未轮询 → 不变
    hr.reload();
    ZEXPECT_EQ(hr.get()->get("v"), std::string("2"));
    std::filesystem::remove(path);
}

/* ---------------- 管理台 REST 端到端 ---------------- */

#ifndef ZENITH_NO_DB
namespace {
struct AdminUser {
    int64_t id = 0;
    std::string name;
    int age = 0;
    std::string city;
};
} // namespace
UORM_REFLECTION(AdminUser, id, name, age, city)

ZTEST(AdminConsole, RestCrudRoundtrip) {
    const std::string dbFile = "zadmin.tmp.sqlite";
    std::filesystem::remove(dbFile);

    core::KeyValueConfig dbCfg;
    dbCfg.set("db.driver", "sqlite");
    dbCfg.set("db.database", dbFile);
    auto dbSvc = std::make_shared<db::DbServiceImpl>(db::DbConfig::fromConfig(dbCfg));
    auto& db = dbSvc->database();
    ZEXPECT_TRUE(db.syncTable<AdminUser>());

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    auto& r = server.router();

    r.get("/", [](http::Context& c) {
        c.setHeader("Content-Type", "text/html; charset=utf-8");
        c.status(200);
        c.response().body = "<html>zenith-admin</html>";
    });
    r.get("/api/users", [&db](http::Context& c) {
        http::json::JsonValue v = http::json::JsonValue::object();
        http::json::JsonValue arr = http::json::JsonValue::array();
        for (const AdminUser& u : db.find<AdminUser>("age >= ?", 0)) {
            http::json::JsonValue o = http::json::JsonValue::object();
            o["id"] = u.id; o["name"] = u.name; o["age"] = u.age; o["city"] = u.city;
            arr.push_back(std::move(o));
        }
        v["users"] = std::move(arr);
        c.json(200, v);
    });
    r.post("/api/users", [&db](http::Context& c) {
        auto in = c.bodyJson();
        AdminUser u{0, in["name"].asString(), static_cast<int>(in["age"].asInt()),
                   in["city"].asString()};
        db.save(u);
        http::json::JsonValue v = http::json::JsonValue::object();
        v["id"] = u.id;
        c.json(201, v);
    });
    r.delete_("/api/users/:id", [&db](http::Context& c) {
        const int64_t id = std::strtoll(c.param("id").c_str(), nullptr, 10);
        auto u = db.findById<AdminUser>(id);
        if (!u.has_value()) { c.status(404).text(404, "404"); return; }
        const bool removed = db.remove(*u);
        c.status(removed ? 200 : 500).text(removed ? 200 : 500, removed ? "deleted" : "fail");
    });

    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(150ms);
    const auto url = [&](const std::string& p) {
        return "http://127.0.0.1:" + std::to_string(server.port()) + p;
    };

    // 首页
    auto home = http::HttpClient::fetchBlockingUrl(url("/"));
    ZEXPECT_EQ(home.value().status, 200);
    ZEXPECT_EQ(home.value().headers.get("Content-Type"), std::string("text/html; charset=utf-8"));
    ZEXPECT_TRUE(home.value().body.find("zenith-admin") != std::string::npos);

    // 空
    auto empty = http::HttpClient::fetchBlockingUrl(url("/api/users"));
    ZEXPECT_EQ(http::json::JsonValue::parse(empty.value().body).value()["users"].size(),
               size_t{0});

    // 建 ×2
    http::ClientRequest mk;
    mk.url = url("/api/users");
    mk.method = http::HttpMethod::Post;
    mk.body = R"({"name":"alice","age":30,"city":"SH"})";
    auto c1 = http::HttpClient::fetchBlocking(mk);
    ZEXPECT_EQ(c1.value().status, 201);
    const int64_t id1 = http::json::JsonValue::parse(c1.value().body).value()["id"].asInt();
    mk.body = R"({"name":"bob","age":22,"city":"BJ"})";
    auto c2 = http::HttpClient::fetchBlocking(mk);
    const int64_t id2 = http::json::JsonValue::parse(c2.value().body).value()["id"].asInt();
    ZEXPECT_TRUE(id1 > 0 && id2 > id1);

    // 列
    auto list = http::HttpClient::fetchBlockingUrl(url("/api/users"));
    ZEXPECT_EQ(http::json::JsonValue::parse(list.value().body).value()["users"].size(),
               size_t{2});

    // 删 → 404 复查
    http::ClientRequest del;
    del.url = url("/api/users/" + std::to_string(id1));
    del.method = http::HttpMethod::Delete;
    auto d = http::HttpClient::fetchBlocking(del);
    ZEXPECT_EQ(d.value().status, 200);
    auto d2 = http::HttpClient::fetchBlocking(del);
    ZEXPECT_EQ(d2.value().status, 404);
    auto list2 = http::HttpClient::fetchBlockingUrl(url("/api/users"));
    ZEXPECT_EQ(http::json::JsonValue::parse(list2.value().body).value()["users"].size(),
               size_t{1});

    loop.stop();
    t.join();
    io.stop();
    // 连接池可能仍短暂持有文件句柄（析构异步收敛）——error_code 版容忍
    std::error_code ec;
    std::filesystem::remove(dbFile, ec);
}
#endif

/* ---------------- WorkerSupervisor（POSIX 门控） ---------------- */

#ifdef ZTEST_HAS_POSIX
ZTEST(WorkerSupervisor, PosixCompileSmoke) {
    net::WorkerSupervisor sup;
    ZEXPECT_EQ(sup.stats().desired, 0);
    // 完整 fork 流程需要真实多进程环境 —— 由 CI Linux 矩阵跑
    // （此处仅验证链接与 API 存在）
}
#else
ZTEST(WorkerSupervisor, WindowsReturnsUnsupported) {
    net::WorkerSupervisor sup;
    auto r = sup.run(1, "127.0.0.1", 0, nullptr);
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_TRUE(r.error().message.find("POSIX only") != std::string::npos);
}
#endif

ZTEST_MAIN
