// ============================================================================
//  zenith::crew::mountCrewApi —— 多 Agent 集群任务 REST + SSE（M11 + M14）
//
//  用户唯一入口 + 实时观测（前端仪表盘就绪）：
//    POST /crew/tasks                开任务 {brief, config?{…CrewConfig 全字段}}
//    GET  /crew/tasks                任务列表
//    GET  /crew/tasks/:id            任务详情（phase/agents/usage/cost 快照）
//    GET  /crew/tasks/:id/board      状态板（前端仪表盘稳定 JSON 模式）
//    GET  /crew/tasks/:id/events?since=N   事件（增量轮询；含 token/retry/cost 流）
//    GET  /crew/tasks/:id/events/stream    SSE 实时流（text/event-stream）
//    GET  /crew/tasks/:id/artifacts  全部产物
//    GET  /crew/tasks/:id/votes      投票记录
//    GET  /crew/tasks/:id/export     全量导出 JSON 包（M14）
//    POST /crew/tasks/:id/approve    人工门禁放行 {?comment}（M14：评论入事件流）
//    POST /crew/tasks/:id/cancel     取消运行中任务（M14）
//    POST /crew/tasks/:id/resume     断点恢复（M14）
//    POST /crew/tasks/:id/followup   增量迭代 {brief, config?}（M16：继承父任务产物）
//    POST /crew/tasks/:id/vote       人工投票 {choice}（M14 humanVoter）
//
//  写操作鉴权（M14）：mountCrewApi(r, svc, token) 非空时，
//  approve/cancel/resume/vote 要求请求头 X-Crew-Token 与之相等。
// ============================================================================
#pragma once

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "zenith/crew/crew.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/security.hpp"
#include "zenith/http/sse.hpp"

namespace zenith::crew {

inline void mountCrewApi(http::Router& r, std::shared_ptr<ICrewService> svc,
                         const std::string& writeToken = {},
                         bool enableCors = true) {
    using namespace http;
    // CORS（ZenithDesk 等跨源前端开发期即开即用；预检路径随路由注册）
    if (enableCors) {
        CorsOptions co;
        co.allowHeaders = "Content-Type, Authorization, X-Request-Id, X-Crew-Token";
        r.use(corsMiddleware(co));
    }
    const bool guarded = !writeToken.empty();
    // 按值捕获：路由生存期长于本函数栈帧
    const std::string token = writeToken;
    auto allowed = [guarded, token](Context& c) {
        if (!guarded) return true;
        return c.request().header("X-Crew-Token") == token;
    };
    // 每个业务路径注册 OPTIONS 占位（CORS 中间件在链中先行短路 204）
    auto preflight = [&r](const std::string& path) {
        r.options(path, [](Context&) {});
    };
    preflight("/crew/tasks");
    preflight("/crew/roster");
    preflight("/crew/board");
    preflight("/crew/meta");
    preflight("/crew/tasks/:id");
    preflight("/crew/tasks/:id/approve");
    preflight("/crew/tasks/:id/cancel");
    preflight("/crew/tasks/:id/resume");
    preflight("/crew/tasks/:id/followup");
    preflight("/crew/tasks/:id/vote");

    r.post("/crew/tasks", [svc](Context& c) {
        auto body = c.bodyJson();
        CrewConfig cfg = CrewConfig::fromJson(body["config"]);
        try {
            const std::string id = svc->createTask(body["brief"].asString(), cfg);
            json::JsonValue v = json::JsonValue::object();
            v["task_id"] = id;
            v["status_url"] = "/crew/tasks/" + id;
            c.json(201, v);
        } catch (const std::exception& e) {
            c.status(503).text(503, std::string("503 ") + e.what());
        }
    });

    r.get("/crew/tasks", [svc](Context& c) {
        json::JsonValue arr = json::JsonValue::array();
        for (const CrewTaskInfo& t : svc->listTasks()) arr.push_back(t.toJson());
        json::JsonValue v = json::JsonValue::object();
        v["tasks"] = std::move(arr);
        c.json(200, v);
    });

    // M15：团队名册——角色↔AI 形象/模型映射（?task= 解析该任务扮演者）
    r.get("/crew/roster", [svc](Context& c) {
        c.json(200, svc->roster(c.query("task")));
    });

    // M15：引擎元信息（特性开关——前端显隐交互元素）
    r.get("/crew/meta", [svc](Context& c) {
        c.json(200, svc->metaInfo());
    });

    // M15：多任务聚合 board（所有运行中/待审批任务的角色拼一张大图）
    r.get("/crew/board", [svc](Context& c) {
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        json::JsonValue arr = json::JsonValue::array();
        for (const CrewTaskInfo& t : svc->listTasks()) {
            if (!t.running && t.phase != CrewPhase::AwaitingApproval) continue;
            auto info = svc->taskInfo(t.id);
            if (!info) continue;
            json::JsonValue j = json::JsonValue::object();
            j["task_id"] = t.id;
            j["phase"] = phaseName(t.phase);
            j["running"] = t.running;
            j["cost_usd"] = t.costUsd;
            json::JsonValue agents = json::JsonValue::array();
            for (const AgentStatus& a : info.value().agents) {
                json::JsonValue aj = a.toJson();
                const std::string model = info.value().roleModels.count(a.role) > 0
                                              ? info.value().roleModels.at(a.role)
                                              : info.value().cfg.model;
                aj["model"] = model;
                aj["persona"] = personaFromModel(model);
                aj["idle_ms"] = (a.state == "working" || a.state == "voting")
                                    ? int64_t{0}
                                    : std::max<int64_t>(0, now - a.updatedMs);
                agents.push_back(std::move(aj));
            }
            j["agents"] = std::move(agents);
            arr.push_back(std::move(j));
        }
        json::JsonValue v = json::JsonValue::object();
        v["tasks"] = std::move(arr);
        c.json(200, v);
    });

    r.get("/crew/tasks/:id", [svc](Context& c) {
        auto info = svc->taskInfo(c.param("id"));
        if (!info) {
            c.status(404).text(404, "404 no such task");
            return;
        }
        c.json(200, info.value().toJson());
    });

    // 状态板：phase + 角色实时快照 + 成本（前端仪表盘直接消费）
    r.get("/crew/tasks/:id/board", [svc](Context& c) {
        auto info = svc->taskInfo(c.param("id"));
        if (!info) {
            c.status(404).text(404, "404 no such task");
            return;
        }
        const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch())
                                .count();
        json::JsonValue v = json::JsonValue::object();
        v["task_id"] = info.value().id;
        v["phase"] = phaseName(info.value().phase);
        v["running"] = info.value().running;
        v["resumable"] = info.value().resumable;
        v["cost_usd"] = info.value().costUsd;
        v["tokens_in"] = static_cast<int64_t>(info.value().tokensIn);
        v["tokens_out"] = static_cast<int64_t>(info.value().tokensOut);
        v["config"] = info.value().cfg.toJson();
        json::JsonValue agents = json::JsonValue::array();
        for (const AgentStatus& a : info.value().agents) {
            json::JsonValue aj = a.toJson();
            const std::string model = info.value().roleModels.count(a.role) > 0
                                          ? info.value().roleModels.at(a.role)
                                          : info.value().cfg.model;
            aj["model"] = model;
            aj["persona"] = personaFromModel(model);
            aj["idle_ms"] = (a.state == "working" || a.state == "voting")
                                ? int64_t{0}
                                : std::max<int64_t>(0, now - a.updatedMs);
            agents.push_back(std::move(aj));
        }
        v["agents"] = std::move(agents);
        v["artifact_count"] = static_cast<int64_t>(info.value().artifactCount);
        v["vote_count"] = static_cast<int64_t>(info.value().voteCount);
        c.json(200, v);
    });

    r.get("/crew/tasks/:id/events", [svc](Context& c) {
        uint64_t since = 0;
        const std::string sq = c.query("since");
        if (!sq.empty()) since = std::strtoull(sq.c_str(), nullptr, 10);
        json::JsonValue arr = json::JsonValue::array();
        uint64_t last = since;
        for (const CrewEvent& e : svc->events(c.param("id"), since)) {
            arr.push_back(e.toJson());
            last = e.seq;
        }
        json::JsonValue v = json::JsonValue::object();
        v["events"] = std::move(arr);
        v["last_seq"] = static_cast<int64_t>(last);
        c.json(200, v);
    });

    // SSE 实时流：订阅 → 推送 → 任务终态后再推尾部并关流
    r.get("/crew/tasks/:id/events/stream", [svc](Context& c) {
        const std::string id = c.param("id");
        auto sse = SseStream::start(c.connection());
        if (sse == nullptr) return;
        sse->retry(3000);

        auto last = std::make_shared<uint64_t>(0);
        for (const CrewEvent& e : svc->events(id, 0)) {
            sse->send(e.toJson().dump(), "crew", std::to_string(e.seq));
            *last = e.seq;
        }

        auto done = std::make_shared<std::atomic<bool>>(false);
        // 订阅回调在编排线程触发：经共享队列 + 本地轮询发送，避免跨线程直写连接
        auto queue = std::make_shared<std::vector<std::pair<uint64_t, std::string>>>();
        auto qmtx = std::make_shared<std::mutex>();
        const uint64_t subId = svc->subscribe(
            id, [last, done, queue, qmtx](const CrewEvent& e) {
                if (*last >= e.seq) return;
                *last = e.seq;
                std::lock_guard<std::mutex> lk(*qmtx);
                queue->emplace_back(e.seq, e.toJson().dump());
                if (e.phase == CrewPhase::Done || e.phase == CrewPhase::Failed ||
                    e.phase == CrewPhase::Cancelled) {
                    done->store(true);
                }
            });
        auto drain = [&]() {
            std::vector<std::pair<uint64_t, std::string>> batch;
            {
                std::lock_guard<std::mutex> lk(*qmtx);
                batch.swap(*queue);
            }
            for (auto& [seq, json] : batch) sse->send(json, "crew", std::to_string(seq));
        };
        drain();
        for (int i = 0; i < 900; ++i) { // 最长挂 3 分钟
            if (sse->closed()) break;
            drain();
            if (done->load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            sse->comment("keep-alive");
        }
        drain();
        svc->unsubscribe(subId);
        sse->send("{\"phase\":\"stream_end\"}", "end");
        sse->close();
    });

    r.get("/crew/tasks/:id/artifacts", [svc](Context& c) {
        json::JsonValue arr = json::JsonValue::array();
        for (const Artifact& a : svc->artifacts(c.param("id"))) {
            arr.push_back(a.toJson());
        }
        json::JsonValue v = json::JsonValue::object();
        v["artifacts"] = std::move(arr);
        c.json(200, v);
    });

    r.get("/crew/tasks/:id/votes", [svc](Context& c) {
        json::JsonValue arr = json::JsonValue::array();
        for (const VoteRecord& v : svc->votes(c.param("id"))) {
            arr.push_back(v.toJson());
        }
        json::JsonValue out = json::JsonValue::object();
        out["votes"] = std::move(arr);
        c.json(200, out);
    });

    // 全量导出（M14）：task + artifacts + votes + events 单包
    r.get("/crew/tasks/:id/export", [svc](Context& c) {
        const std::string bundle = svc->exportBundle(c.param("id"));
        if (bundle.empty()) {
            c.status(404).text(404, "404 no such task");
            return;
        }
        c.response().headers.set("Content-Type", "application/json; charset=utf-8");
        c.response().headers.set("Content-Disposition",
                                 "attachment; filename=\"" + c.param("id") + ".json\"");
        c.response().body = bundle;
    });

    r.post("/crew/tasks/:id/approve", [svc, allowed](Context& c) {
        if (!allowed(c)) {
            c.status(401).text(401, "401 bad crew token");
            return;
        }
        std::string comment;
        if (!c.request().body.empty()) {
            comment = c.bodyJson()["comment"].asString();
        }
        auto res = svc->approve(c.param("id"), comment);
        c.status(res ? 200 : 404).text(res ? 200 : 404,
                                       res ? "ok" : "404 no such task");
    });

    r.post("/crew/tasks/:id/cancel", [svc, allowed](Context& c) {
        if (!allowed(c)) {
            c.status(401).text(401, "401 bad crew token");
            return;
        }
        auto res = svc->cancel(c.param("id"));
        c.status(res ? 200 : 409).text(res ? 200 : 409,
                                       res ? "cancelled" : "409 " + res.error().message);
    });

    r.post("/crew/tasks/:id/resume", [svc, allowed](Context& c) {
        if (!allowed(c)) {
            c.status(401).text(401, "401 bad crew token");
            return;
        }
        auto res = svc->resume(c.param("id"));
        c.status(res ? 200 : 409).text(res ? 200 : 409,
                                       res ? "resumed" : "409 " + res.error().message);
    });

    // M16：增量迭代——基于父任务既有产物开新任务
    r.post("/crew/tasks/:id/followup", [svc, allowed](Context& c) {
        if (!allowed(c)) {
            c.status(401).text(401, "401 bad crew token");
            return;
        }
        if (c.request().body.empty()) {
            c.status(400).text(400, "400 brief required");
            return;
        }
        auto body = c.bodyJson();
        CrewConfig cfg = CrewConfig::fromJson(body["config"]);
        try {
            const std::string id =
                svc->createFollowupTask(c.param("id"), body["brief"].asString(), cfg);
            if (id.empty()) {
                c.status(404).text(404, "404 no such parent task");
                return;
            }
            json::JsonValue v = json::JsonValue::object();
            v["task_id"] = id;
            v["parent_task_id"] = c.param("id");
            v["status_url"] = "/crew/tasks/" + id;
            c.json(201, v);
        } catch (const std::exception& e) {
            c.status(404).text(404, std::string("404 ") + e.what());
        }
    });

    r.post("/crew/tasks/:id/vote", [svc, allowed](Context& c) {
        if (!allowed(c)) {
            c.status(401).text(401, "401 bad crew token");
            return;
        }
        if (c.request().body.empty()) {
            c.status(400).text(400, "400 choice required");
            return;
        }
        auto res = svc->castHumanVote(c.param("id"), c.bodyJson()["choice"].asString());
        c.status(res ? 200 : 404).text(res ? 200 : 404,
                                       res ? "ok" : "404 no such task");
    });
}

// ============================================================================
//  mountCrewWs —— /crew/ws WebSocket 端点（M15；ZenithDesk 强交互通道）
//
//  协议（文本 JSON 帧）：
//    客户端 → 服务端：
//      {"type":"watch","task":"crew-1-x"[,"token":"..."]}   订阅任务（可换台）
//      {"type":"approve","task":...,"comment":"..."}        人工放行
//      {"type":"cancel","task":...}                         取消
//      {"type":"resume","task":...}                         断点恢复
//      {"type":"vote","task":...,"choice":"..."}            人工投票
//      {"type":"ping"}                                       心跳
//    服务端 → 客户端：
//      {"type":"hello","task":...}                          订阅确认（附历史事件）
//      CrewEvent.toJson() 原样逐帧推送（与 SSE "crew" 事件同构）
//      {"type":"ack","action":...,"ok":bool,"error":...}    动作回执
//      {"type":"pong"}
//  写操作鉴权同 REST：guard 非空时帧内 token 字段必须匹配。
// ============================================================================
inline void mountCrewWs(http::HttpServer& srv, std::shared_ptr<ICrewService> svc,
                        const std::string& writeToken = {}) {
    struct WatchState {
        std::string task;
        uint64_t subId = 0;
    };
    srv.enableWebSocket("/crew/ws",
                        [svc, writeToken](const net::WebSocketSession::Ptr& s) {
        auto watch = std::make_shared<WatchState>();
        s->setContext(watch);

        s->setMessageCallback(
            [svc, s, watch, writeToken](const net::WebSocketSession::Ptr&,
                                        std::string msg, bool) {
                auto v = json::JsonValue::parse(msg);
                if (!v) return;
                const std::string type = v.value()["type"].asString();
                const std::string task = v.value()["task"].asString();
                const bool authed =
                    writeToken.empty() ||
                    v.value()["token"].asString() == writeToken;

                auto ack = [&s](const std::string& action, bool ok,
                                const std::string& err = {}) {
                    json::JsonValue a = json::JsonValue::object();
                    a["type"] = "ack";
                    a["action"] = action;
                    a["ok"] = ok;
                    if (!err.empty()) a["error"] = err;
                    s->sendText(a.dump());
                };

                if (type == "ping") {
                    json::JsonValue p = json::JsonValue::object();
                    p["type"] = "pong";
                    s->sendText(p.dump());
                    return;
                }
                if (type == "watch") {
                    if (watch->subId != 0) {
                        svc->unsubscribe(watch->subId);
                        watch->subId = 0;
                    }
                    watch->task = task;
                    json::JsonValue h = json::JsonValue::object();
                    h["type"] = "hello";
                    h["task"] = task;
                    s->sendText(h.dump());
                    // 订阅前先补全量历史（回放），再实时订阅
                    for (const CrewEvent& e : svc->events(task, 0)) {
                        s->sendText(e.toJson().dump());
                    }
                    watch->subId = svc->subscribe(task, [s](const CrewEvent& e) {
                        s->sendText(e.toJson().dump()); // runInLoop 投递，跨线程安全
                    });
                    return;
                }
                if (type == "approve") {
                    if (!authed) return ack(type, false, "bad token");
                    auto r = svc->approve(task, v.value()["comment"].asString());
                    return ack(type, static_cast<bool>(r));
                }
                if (type == "cancel") {
                    if (!authed) return ack(type, false, "bad token");
                    auto r = svc->cancel(task);
                    return ack(type, static_cast<bool>(r),
                               r ? std::string() : r.error().message);
                }
                if (type == "resume") {
                    if (!authed) return ack(type, false, "bad token");
                    auto r = svc->resume(task);
                    return ack(type, static_cast<bool>(r),
                               r ? std::string() : r.error().message);
                }
                if (type == "vote") {
                    if (!authed) return ack(type, false, "bad token");
                    auto r = svc->castHumanVote(task, v.value()["choice"].asString());
                    return ack(type, static_cast<bool>(r));
                }
            });

        s->setCloseCallback([svc, watch](const net::WebSocketSession::Ptr&) {
            if (watch->subId != 0) {
                svc->unsubscribe(watch->subId);
                watch->subId = 0;
            }
        });
    });
}

} // namespace zenith::crew
