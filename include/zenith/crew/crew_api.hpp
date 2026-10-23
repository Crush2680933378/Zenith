// ============================================================================
//  zenith::crew::mountCrewApi —— 多 Agent 集群任务 REST + SSE（M11）
//
//  用户唯一入口 + 实时观测（前端仪表盘就绪）：
//    POST /crew/tasks                开任务 {brief, config?{model,max_bug_cycles,require_approval}}
//    GET  /crew/tasks                任务列表
//    GET  /crew/tasks/:id            任务详情（phase/agents 快照）
//    GET  /crew/tasks/:id/board      状态板（前端仪表盘稳定 JSON 模式）
//    GET  /crew/tasks/:id/events?since=N   事件（增量轮询）
//    GET  /crew/tasks/:id/events/stream    SSE 实时流（text/event-stream）
//    GET  /crew/tasks/:id/artifacts  全部产物
//    GET  /crew/tasks/:id/votes      投票记录
//    POST /crew/tasks/:id/approve    人工门禁放行
// ============================================================================
#pragma once

#include <chrono>
#include <mutex>
#include <utility>
#include <memory>
#include <thread>

#include "zenith/http/http_server.hpp"
#include "zenith/http/sse.hpp"
#include "zenith/crew/crew.hpp"

namespace zenith::crew {

inline void mountCrewApi(http::Router& r, std::shared_ptr<ICrewService> svc) {
    using namespace http;

    r.post("/crew/tasks", [svc](Context& c) {
        auto body = c.bodyJson();
        CrewConfig cfg;
        cfg.model = body["config"]["model"].asString();
        cfg.maxBugCycles = static_cast<int>(body["config"]["max_bug_cycles"].asInt(2));
        cfg.requireApproval = body["config"]["require_approval"].asBool(false);
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

    r.get("/crew/tasks/:id", [svc](Context& c) {
        auto info = svc->taskInfo(c.param("id"));
        if (!info) {
            c.status(404).text(404, "404 no such task");
            return;
        }
        c.json(200, info.value().toJson());
    });

    // 状态板：phase + 7 角色实时快照（前端仪表盘直接消费）
    r.get("/crew/tasks/:id/board", [svc](Context& c) {
        auto info = svc->taskInfo(c.param("id"));
        if (!info) {
            c.status(404).text(404, "404 no such task");
            return;
        }
        json::JsonValue v = json::JsonValue::object();
        v["task_id"] = info.value().id;
        v["phase"] = phaseName(info.value().phase);
        v["running"] = info.value().running;
        json::JsonValue agents = json::JsonValue::array();
        for (const AgentStatus& a : info.value().agents) agents.push_back(a.toJson());
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

        // 先补历史（since=0 起点全量），再实时订阅
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
                if (e.phase == CrewPhase::Done || e.phase == CrewPhase::Failed) {
                    done->store(true);
                }
            });
        // 排空队列（订阅期 + 收尾期统一在本线程写 SSE——连接单线程写）
        auto drain = [&]() {
            std::vector<std::pair<uint64_t, std::string>> batch;
            {
                std::lock_guard<std::mutex> lk(*qmtx);
                batch.swap(*queue);
            }
            for (auto& [seq, json] : batch) sse->send(json, "crew", std::to_string(seq));
        };
        for (const CrewEvent& e : svc->events(id, 0)) {
            // 历史已在上方直发；drain 只管订阅后的增量
        }
        drain();

        // 轮询式转发：单线程写连接；终态/断开即收尾
        for (int i = 0; i < 600; ++i) { // 最长挂 5 分钟
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
        for (const VoteRecord& v : svc->votes(c.param("id"))) arr.push_back(v.toJson());
        json::JsonValue out = json::JsonValue::object();
        out["votes"] = std::move(arr);
        c.json(200, out);
    });

    r.post("/crew/tasks/:id/approve", [svc](Context& c) {
        auto res = svc->approve(c.param("id"));
        c.json(res ? 200 : 404, json::JsonValue());
        c.response().body = res ? "{\"ok\":true}" : "{\"ok\":false}";
    });
}

} // namespace zenith::crew
