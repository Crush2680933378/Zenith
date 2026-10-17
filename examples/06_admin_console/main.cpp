// ============================================================================
//  06_admin_console —— DB 管理台（M5.6c）
//  Zenith 自研 HTTP 承载（替换 uORM 自带 asio 控制台的旧愿）：
//    - 后端：REST(uORM/sqlite：User 实体 CRUD + 统计) + 可观测三件套
//    - 前端：内嵌单文件控制台（原生 JS，无构建链）
//  运行：./admin_demo → http://127.0.0.1:8090
// ============================================================================
#include <memory>
#include <thread>

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/db/db_service.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/http/security.hpp"
#include "zenith/http/session.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/net/signal.hpp"

namespace {

// uORM 实体
struct User {
    int64_t id = 0;
    std::string name;
    int age = 0;
    std::string city;
};
} // namespace
UORM_REFLECTION(User, id, name, age, city)

// 内嵌控制台（单文件 HTML；用原始字符串避免转义地狱）
static const char* kConsoleHtml = R"HTML(<!DOCTYPE html>
<html lang="zh"><head><meta charset="utf-8">
<title>Zenith Admin</title><style>
body{font-family:system-ui;margin:0;background:#0f1420;color:#e2e8f0}
header{padding:14px 24px;background:#151d2e;border-bottom:1px solid #26304a}
h1{font-size:18px;margin:0} .sub{color:#7c8aa5;font-size:12px}
main{padding:24px;display:grid;grid-template-columns:240px 1fr;gap:24px}
nav button{display:block;width:100%;margin:6px 0;padding:10px;background:#1a2438;
 color:#e2e8f0;border:1px solid #2b3650;border-radius:8px;cursor:pointer;text-align:left}
nav button.active{background:#2456c8;border-color:#3768e8}
table{width:100%;border-collapse:collapse;background:#141c2d;border-radius:10px;overflow:hidden}
th,td{padding:10px 14px;border-bottom:1px solid #212b42;text-align:left;font-size:14px}
th{background:#182238;color:#8fa1c0}
input{background:#0d1420;color:#e2e8f0;border:1px solid #2b3650;border-radius:6px;padding:8px}
button.op{background:#2456c8;color:#fff;border:0;border-radius:6px;padding:8px 14px;cursor:pointer}
button.op:hover{background:#3768e8}
.bar{display:flex;gap:10px;margin-bottom:14px;align-items:center}
.card{background:#141c2d;border-radius:10px;padding:16px;margin-bottom:14px}
.stat{display:inline-block;margin-right:32px} .stat b{font-size:22px;display:block}
</style></head><body>
<header><h1>Zenith Admin Console</h1>
<div class="sub">zenith::http + zenith::db(uORM/sqlite) — 自研栈承载</div></header>
<main><nav>
<button id="navUsers" class="active">Users</button>
<button id="navStats">Server Stats</button>
</nav><section>
<div id="usersPane">
<div class="bar">
 <input id="fName" placeholder="name">
 <input id="fAge" placeholder="age" size="6">
 <input id="fCity" placeholder="city">
 <button class="op" onclick="addUser()">Add</button>
 <input id="fSearch" placeholder="search name..." oninput="loadUsers()">
</div>
<table><thead><tr><th>ID</th><th>Name</th><th>Age</th><th>City</th><th></th></tr></thead>
<tbody id="userRows"></tbody></table></div>
<div id="statsPane" style="display:none">
<div class="card" id="statsCard">loading…</div>
<button class="op" onclick="loadStats()">Refresh</button></div>
</section></main>
<script>
const $=id=>document.getElementById(id);
async function loadUsers(){
 const q=$('fSearch').value, r=await fetch('/api/users'+(q?('?q='+encodeURIComponent(q)):''));
 const d=await r.json();
 $('userRows').innerHTML=d.users.map(u=>`<tr><td>${u.id}</td><td>${u.name}</td>
  <td>${u.age}</td><td>${u.city}</td>
  <td><button class="op" onclick="delUser(${u.id})">del</button></td></tr>`).join('');
}
async function addUser(){
 const age=parseInt($('fAge').value||'0');
 await fetch('/api/users',{method:'POST',headers:{'Content-Type':'application/json'},
  body:JSON.stringify({name:$('fName').value||'unnamed',age:age||18,city:$('fCity').value||'-'})});
 $('fName').value=$('fAge').value=$('fCity').value='';loadUsers();
}
async function delUser(id){await fetch('/api/users/'+id,{method:'DELETE'});loadUsers();}
async function loadStats(){
 const r=await fetch('/zenith/status');const d=await r.json();
 $('statsCard').innerHTML=
  `<div class="stat"><b>${d.http_requests_total}</b>requests</div>
   <div class="stat"><b>${d.http_errors_total}</b>errors</div>
   <div class="stat"><b>${d.http_inflight}</b>in-flight</div>
   <div class="stat"><b>${Math.round(d.uptime_ms/1000)}s</b>uptime</div>
   <div class="stat"><b>${d.latency.count}</b>samples p99=${d.latency.p99_sec}s</div>`;
}
$('navUsers').onclick=()=>{toggle(true)};
$('navStats').onclick=()=>{toggle(false);loadStats()};
function toggle(users){
 $('usersPane').style.display=users?'':'none';
 $('statsPane').style.display=users?'none':'';
 $('navUsers').classList.toggle('active',users);
 $('navStats').classList.toggle('active',!users);
}
loadUsers();
</script></body></html>)HTML";

int main() {
    using namespace zenith;

    auto& facade = log::LoggerFacade::instance();
    facade.setLevel(log::LogLevel::Info);
    facade.addSink(std::make_shared<log::ConsoleLogger>(true, true));

    // 数据层：sqlite 文件库（M3 的 zenith::db 服务化）
    core::KeyValueConfig dbCfg;
    dbCfg.set("db.driver", "sqlite");
    dbCfg.set("db.database", "admin_demo.sqlite");
    auto dbSvc = std::make_shared<db::DbServiceImpl>(db::DbConfig::fromConfig(dbCfg));
    auto& db = dbSvc->database();
    if (!db.syncTable<User>()) {
        ZLOG_ERROR("syncTable<User> failed");
        return 1;
    }
    if (db.find<User>("age >= ?", 0).empty()) { // 首启种子数据
        std::vector<User> seed{
            {0, "alice", 30, "Shanghai"}, {0, "bob", 22, "Beijing"}, {0, "carol", 27, "Shenzhen"}};
        db.saveRange(seed);
    }

    net::EventLoop loop;
    net::EventLoopThreadPool io(2);
    io.start();
    http::HttpServer server(&loop, &io);
    auto& r = server.router();

    r.use(http::requestIdMiddleware());
    r.use(http::metricsMiddleware());
    r.use(http::securityHeadersMiddleware());

    // 控制台首页（内嵌 HTML）
    r.get("/", [](http::Context& c) {
        c.setHeader("Content-Type", "text/html; charset=utf-8");
        c.status(200);
        c.response().body = kConsoleHtml;
    });

    // ---- REST：User CRUD ----
    r.get("/api/users", [&db](http::Context& c) {
        http::json::JsonValue v = http::json::JsonValue::object();
        http::json::JsonValue arr = http::json::JsonValue::array();
        const std::string q = c.query("q");
        std::vector<User> rows = q.empty() ? db.find<User>("age >= ?", 0)
                                           : db.find<User>("name LIKE ?", "%" + q + "%");
        for (const User& u : rows) {
            http::json::JsonValue o = http::json::JsonValue::object();
            o["id"] = u.id;
            o["name"] = u.name;
            o["age"] = u.age;
            o["city"] = u.city;
            arr.push_back(std::move(o));
        }
        v["users"] = std::move(arr);
        c.json(200, v);
    });
    r.post("/api/users", [&db](http::Context& c) {
        auto in = c.bodyJson();
        User u{0, in["name"].asString(), static_cast<int>(in["age"].asInt()),
               in["city"].asString()};
        if (!db.save(u)) {
            c.status(500).text(500, "save failed");
            return;
        }
        http::json::JsonValue v = http::json::JsonValue::object();
        v["id"] = u.id;
        c.json(201, v);
    });
    r.delete_("/api/users/:id", [&db](http::Context& c) {
        const int64_t id = std::strtoll(c.param("id").c_str(), nullptr, 10);
        auto u = db.findById<User>(id);
        if (!u.has_value()) {
            c.status(404).text(404, "404 no such user");
            return;
        }
        c.status(db.remove(*u) ? 200 : 500).noContent();
    });

    r.get("/metrics", http::prometheusHandler());
    r.get("/zenith/status", http::statusHandler());

    if (!server.listen("0.0.0.0", 8090)) return 1;
    ZLOG_INFO("Zenith admin console on http://127.0.0.1:8090 (sqlite: admin_demo.sqlite)");

    net::installSignalHandler([&] {
        ZLOG_INFO("shutting down...");
        server.beginGracefulShutdown([&] { loop.stop(); });
    });
    loop.loop();
    io.stop();
    facade.flush();
    facade.shutdown();
    return 0;
}
