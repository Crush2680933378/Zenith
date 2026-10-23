// M11 测试：多 Agent 集群——全流程/投票/缺陷环/人工门禁/SSE/事件流
#include <atomic>
#include <set>
#include <chrono>
#include <thread>

#include "ztest.hpp"

#include "zenith/crew/crew.hpp"
#include "zenith/crew/crew_api.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace zenith::crew;
using namespace std::chrono_literals;

namespace {

/// 剧本推理：按 system prompt 中的角色标记 + 轮次返回确定 JSON
class CrewScripted final : public ai::IInferenceService {
public:
    std::vector<ai::ModelInfo> models() override { return {}; }
    ai::ChatResult chat(const ai::ChatRequest& req,
                        const ai::TokenCallback& = nullptr,
                        const ai::TokenCallback& = nullptr) override {
        const std::string& sys =
            req.messages.empty() ? "" : req.messages.front().content;
        const std::string& user =
            req.messages.size() < 2 ? "" : req.messages.back().content;
        ai::ChatResult r;
        r.ok = true;
        r.model = "crew-scripted";

        if (sys.find("VOTE:") != std::string::npos) {
            // PRD 评审：backend 投 revise（首轮），其余 approve → approve 胜出
            // 存储选型：全员 sqlite（架构师加权仍 sqlite）
            if (sys.find("acceptance criteria") != std::string::npos) {
                if (sys.find("backend") != std::string::npos && !revoted_) {
                    revoted_ = true;
                    r.content = R"({"choice":"revise","reason":"criteria vague"})";
                } else {
                    r.content = R"({"choice":"approve","reason":"looks good"})";
                }
            } else {
                r.content = R"({"choice":"sqlite","reason":"zero-ops for demo"})";
            }
            return r;
        }
        if (sys.find("ROLE: PM") != std::string::npos) {
            r.content = R"({"prd":{"summary":"demo","stories":[{"id":"S1","title":"create","given":"empty","when":"post","then":"created"}]}})";
            return r;
        }
        if (sys.find("ROLE: ARCHITECT") != std::string::npos) {
            r.content = R"({"architecture":{"style":"rest","storage":"sqlite","components":["api"],"api":[{"method":"GET","path":"/api/items","desc":"list"}],"notes":"n"}})";
            return r;
        }
        if (sys.find("ROLE: BACKEND") != std::string::npos) {
            r.content = R"({"files":[{"path":"src/main.cpp","content":"// impl"}],"notes":"done"})";
            return r;
        }
        if (sys.find("ROLE: FRONTEND") != std::string::npos) {
            r.content = R"({"files":[{"path":"web/index.html","content":"<b>ui</b>"}],"notes":"done"})";
            return r;
        }
        if (sys.find("ROLE: TESTER") != std::string::npos) {
            if (user.find("RETEST") == std::string::npos && !retested_) {
                retested_ = true;
                r.content = R"({"cases":[{"id":"T1","story":"S1","given":"e","when":"p","then":"c"}],"results":[{"case":"T1","status":"fail","detail":"500","component":"backend"}]})";
            } else {
                r.content = R"({"cases":[{"id":"T1","story":"S1","given":"e","when":"p","then":"c"}],"results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"}]})";
            }
            return r;
        }
        if (sys.find("ROLE: DEVOPS") != std::string::npos) {
            r.content = R"({"files":[{"path":"Dockerfile","content":"FROM scratch"}],"deploy":{"strategy":"docker","smoke":"GET /api/items"}})";
            return r;
        }
        if (sys.find("ROLE: DELIVERY") != std::string::npos) {
            r.content = R"({"changelog":"# v0.1.0","release_notes":"first","version":"0.1.0"})";
            return r;
        }
        r.content = "{}";
        return r;
    }
    ai::EmbeddingResult embeddings(const std::string&, const std::string&) override {
        ai::EmbeddingResult e;
        e.ok = false;
        return e;
    }
    std::string backend() const override { return "crew-scripted"; }

    bool revoted_ = false;
    bool retested_ = false;
};

CrewTaskInfo waitDone(crew::CrewEngine& eng, const std::string& id, int maxSec = 30) {
    for (int i = 0; i < maxSec * 10; ++i) {
        auto info = eng.taskInfo(id);
        if (info && !info.value().running &&
            (info.value().phase == crew::CrewPhase::Done ||
             info.value().phase == crew::CrewPhase::Failed ||
             info.value().phase == crew::CrewPhase::AwaitingApproval)) {
            return info.value();
        }
        std::this_thread::sleep_for(100ms);
    }
    auto info = eng.taskInfo(id);
    return info ? info.value() : CrewTaskInfo{};
}

} // namespace

ZTEST(Crew, FullLifecycleWithVotingAndBugLoop) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<CrewScripted>());

    // 订阅实时事件（SSE 底层同路径）
    std::atomic<int> received{0};
    const std::string id = eng.createTask("team voting app");
    const uint64_t sub = eng.subscribe(id, [&](const crew::CrewEvent&) {
        received.fetch_add(1);
    });

    const CrewTaskInfo final = waitDone(eng, id);
    ZEXPECT_EQ(phaseName(final.phase), std::string("done"));
    ZEXPECT_TRUE(final.error.empty());

    // 7 类产物齐备（prd 可能 v2 因 revise、bugs 因缺陷环）
    auto arts = eng.artifacts(id);
    std::set<std::string> types;
    for (const auto& a : arts) types.insert(a.type);
    for (const char* need :
         {"prd", "architecture", "contract", "backend", "frontend", "tests", "bugs",
          "devops", "release"}) {
        ZEXPECT_TRUE(types.count(need) > 0);
    }

    // 投票 ≥2（PRD 评审 + 存储选型），存储胜出 sqlite
    auto vs = eng.votes(id);
    ZEXPECT_TRUE(vs.size() >= 2);
    bool storageVote = false;
    for (const auto& v : vs) {
        if (v.question.find("storage") != std::string::npos && v.winner == "sqlite") {
            storageVote = true;
            ZEXPECT_TRUE(v.votes.size() == 7); // 全员投票
        }
    }
    ZEXPECT_TRUE(storageVote);

    // 事件序列单调且覆盖全部阶段
    auto evs = eng.events(id);
    ZEXPECT_TRUE(evs.size() >= 15);
    bool sawBugfix = false, sawVote = false, sawArtifact = false;
    for (size_t i = 1; i < evs.size(); ++i) {
        ZEXPECT_TRUE(evs[i].seq > evs[i - 1].seq);
    }
    for (const auto& e : evs) {
        if (e.phase == crew::CrewPhase::Bugfix) sawBugfix = true;
        if (e.type == "vote") sawVote = true;
        if (e.type == "artifact") sawArtifact = true;
    }
    ZEXPECT_TRUE(sawBugfix); // 缺陷环走过（首轮 fail → 后端修复 → 重测过）
    ZEXPECT_TRUE(sawVote);
    ZEXPECT_TRUE(sawArtifact);

    // 状态板：7 角色快照
    ZEXPECT_EQ(final.agents.size(), size_t{7});

    // 订阅回调确实收到了实时事件
    eng.unsubscribe(sub);
    std::this_thread::sleep_for(200ms);
    ZEXPECT_TRUE(received.load() >= 5);
}

ZTEST(Crew, ApprovalGateBlocksAndResumes) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<CrewScripted>());

    crew::CrewConfig cfg;
    cfg.requireApproval = true;
    const std::string id = eng.createTask("gated task", cfg);

    CrewTaskInfo st = waitDone(eng, id);
    ZEXPECT_EQ(phaseName(st.phase), std::string("awaiting_approval")); // 设计后停住

    // 事件里有 approval_required
    bool sawGate = false;
    for (const auto& e : eng.events(id)) {
        if (e.type == "approval") sawGate = true;
    }
    ZEXPECT_TRUE(sawGate);

    ZEXPECT_TRUE(static_cast<bool>(eng.approve(id)));
    const CrewTaskInfo done = waitDone(eng, id);
    ZEXPECT_EQ(phaseName(done.phase), std::string("done"));
}

ZTEST(Crew, VoteMechanismWeightedMajority) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<CrewScripted>());

    const std::string id = eng.createTask("vote demo");
    waitDone(eng, id); // 等任务完（投票表建立）

    // 直接单次投票：全员对 sqlite/postgres——剧本恒投 sqlite
    crew::CrewEngine::VoteSpec spec;
    spec.question = "storage?";
    spec.options = {"sqlite", "postgres"};
    spec.voters = {"pm", "architect", "tester"};
    auto rec = eng.runVote(id, spec);
    ZEXPECT_EQ(rec.winner, std::string("sqlite"));
    ZEXPECT_EQ(rec.votes.size(), size_t{3});
    // 架构师加权：sqlite 得 1+2+1=4
    ZEXPECT_TRUE(rec.reason.find("zero-ops") != std::string::npos ||
                 rec.reason.find("majority") != std::string::npos);
}

ZTEST(Crew, HttpRestAndBoard) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<CrewScripted>());
    const std::string id = eng.createTask("http demo");
    waitDone(eng, id);

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    // 轻量引用包装（插件场景来自 ServiceRegistry；测试直接包引擎引用）
    class CrewSvcRef final : public crew::ICrewService {
    public:
        explicit CrewSvcRef(crew::CrewEngine& e) : e_(e) {}
        std::string createTask(const std::string& b, const crew::CrewConfig& c) override {
            return e_.createTask(b, c);
        }
        core::Result<crew::CrewTaskInfo> taskInfo(const std::string& i) override {
            return e_.taskInfo(i);
        }
        std::vector<crew::CrewTaskInfo> listTasks() override { return e_.listTasks(); }
        std::vector<crew::CrewEvent> events(const std::string& i,
                                            uint64_t s) override {
            return e_.events(i, s);
        }
        std::vector<crew::Artifact> artifacts(const std::string& i) override {
            return e_.artifacts(i);
        }
        std::vector<crew::VoteRecord> votes(const std::string& i) override {
            return e_.votes(i);
        }
        core::Result<void> approve(const std::string& i) override {
            return e_.approve(i);
        }
        uint64_t subscribe(const std::string& t, EventCallback cb) override {
            return e_.subscribe(t, std::move(cb));
        }
        void unsubscribe(uint64_t s) override { e_.unsubscribe(s); }
        void setInference(std::shared_ptr<ai::IInferenceService>) override {}

    private:
        crew::CrewEngine& e_;
    };
    crew::mountCrewApi(server.router(), std::make_shared<CrewSvcRef>(eng));
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(150ms);
    const auto url = [&](const std::string& p) {
        return "http://127.0.0.1:" + std::to_string(server.port()) + p;
    };

    // 详情
    auto info = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/" + id));
    ZEXPECT_EQ(info.value().status, 200);
    auto iv = http::json::JsonValue::parse(info.value().body);
    ZEXPECT_TRUE(static_cast<bool>(iv));
    ZEXPECT_EQ(iv.value()["phase"].asString(), std::string("done"));
    ZEXPECT_EQ(iv.value()["agents"].size(), size_t{7});

    // 状态板（前端仪表盘模式）
    auto board = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/" + id + "/board"));
    ZEXPECT_EQ(board.value().status, 200);
    auto bv = http::json::JsonValue::parse(board.value().body);
    ZEXPECT_TRUE(static_cast<bool>(bv));
    ZEXPECT_EQ(bv.value()["phase"].asString(), std::string("done"));
    ZEXPECT_TRUE(bv.value().has("agents"));

    // 事件增量轮询
    auto ev1 = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/" + id + "/events"));
    auto e1 = http::json::JsonValue::parse(ev1.value().body);
    ZEXPECT_TRUE(static_cast<bool>(e1));
    const int64_t total = e1.value()["events"].size();
    ZEXPECT_TRUE(total >= 15);
    const int64_t lastSeq = e1.value()["last_seq"].asInt();
    auto ev2 = http::HttpClient::fetchBlockingUrl(
        url("/crew/tasks/" + id + "/events?since=" + std::to_string(lastSeq)));
    auto e2 = http::json::JsonValue::parse(ev2.value().body);
    ZEXPECT_EQ(e2.value()["events"].size(), size_t{0}); // 增量为空

    // 产物 + 投票
    auto arts = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/" + id + "/artifacts"));
    ZEXPECT_EQ(arts.value().status, 200);
    auto vs = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/" + id + "/votes"));
    auto vv = http::json::JsonValue::parse(vs.value().body);
    ZEXPECT_TRUE(vv.value()["votes"].size() >= 2);

    // 404
    auto nf = http::HttpClient::fetchBlockingUrl(url("/crew/tasks/nope"));
    ZEXPECT_EQ(nf.value().status, 404);

    loop.stop();
    t.join();
    io.stop();
}

ZTEST_MAIN
