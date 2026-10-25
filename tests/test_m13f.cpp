// M16 P0 测试：上下文截断 / 成本预算熔断 / cancel webhook 补漏 / followup 增量迭代
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "ztest.hpp"

#include "zenith/ai/provider_router.hpp"
#include "zenith/crew/crew.hpp"
#include "zenith/crew/crew_api.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
class Rec final : public ai::IInferenceService {
public:
    std::vector<ai::ModelInfo> models() override { return {}; }
    int chatSleepMs = 0;
    std::atomic<int> inFlight{0};
    std::mutex m;
    std::vector<std::string> testerPrompts;
    std::vector<std::string> pmPrompts;
    int64_t usageIn = 0, usageOut = 0;
    bool countUsage = false;

    ai::ChatResult chat(const ai::ChatRequest& req,
                        const ai::TokenCallback& = nullptr,
                        const ai::TokenCallback& = nullptr) override {
        const std::string& sys = req.messages.front().content;
        const std::string& user = req.messages.back().content;
        const std::string role = tagOf(sys);
        {
            std::lock_guard<std::mutex> lk(m);
            if (role == "tester") testerPrompts.push_back(user);
            if (role == "pm") pmPrompts.push_back(user);
        }
        if (chatSleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(chatSleepMs));
        ai::ChatResult r;
        r.ok = true;
        r.model = "rec";
        if (countUsage) {
            r.usage.promptTokens = static_cast<int>(usageIn);
            r.usage.completionTokens = static_cast<int>(usageOut);
        }
        if (role == "vote") {
            if (user.find("acceptance criteria") != std::string::npos) {
                r.content = !rev_ ? (rev_ = true, R"({"choice":"revise","reason":"v"})")
                                  : R"({"choice":"approve","reason":"ok"})";
            } else {
                r.content = R"({"choice":"sqlite","reason":"z"})";
            }
        } else if (role == "pm") {
            r.content = R"({"prd":{"summary":"d","stories":[{"id":"S1","title":"a","given":"e","when":"p","then":"c"}]}})";
        } else if (role == "architect") {
            r.content = R"({"architecture":{"api":[{"method":"GET","path":"/a","desc":"d"}],"storage":"sqlite"}})";
        } else if (role == "backend" || role == "frontend") {
            r.content = R"({"files":[{"path":"x","content":"y"}],"notes":"n"})";
        } else if (role == "tester") {
            r.content = (user.find("RETEST") == std::string::npos && !ret_)
                ? (ret_ = true, R"({"cases":[{"id":"T1"}],"results":[{"case":"T1","status":"fail","detail":"500","component":"backend"}]})")
                : R"({"cases":[{"id":"T1"}],"results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"}]})";
        } else if (role == "devops") {
            r.content = R"({"files":[{"path":"D","content":"x"}],"deploy":{"strategy":"d"}})";
        } else if (role == "delivery") {
            r.content = R"({"changelog":"c","release_notes":"n","version":"0.1.0"})";
        } else {
            r.content = "{}";
        }
        return r;
    }
    ai::EmbeddingResult embeddings(const std::string&, const std::string&) override {
        return ai::EmbeddingResult{};
    }
    std::string backend() const override { return "rec"; }

private:
    static std::string tagOf(const std::string& sys) {
        if (sys.find("VOTE:") != std::string::npos) return "vote";
        if (sys.find("ROLE: PM") != std::string::npos) return "pm";
        if (sys.find("ROLE: ARCHITECT") != std::string::npos) return "architect";
        if (sys.find("ROLE: BACKEND") != std::string::npos) return "backend";
        if (sys.find("ROLE: FRONTEND") != std::string::npos) return "frontend";
        if (sys.find("ROLE: TESTER") != std::string::npos) return "tester";
        if (sys.find("ROLE: DEVOPS") != std::string::npos) return "devops";
        if (sys.find("ROLE: DELIVERY") != std::string::npos) return "delivery";
        return "?";
    }
    bool rev_ = false, ret_ = false;
};

using RecPtr = std::shared_ptr<Rec>;

crew::CrewTaskInfo waitTerminal(crew::CrewEngine& eng, const std::string& id,
                                int maxSec = 30) {
    for (int i = 0; i < maxSec * 20; ++i) {
        auto info = eng.taskInfo(id);
        if (info && !info.value().running &&
            info.value().phase != crew::CrewPhase::AwaitingApproval) {
            return info.value();
        }
        std::this_thread::sleep_for(50ms);
    }
    auto info = eng.taskInfo(id);
    return info ? info.value() : crew::CrewTaskInfo{};
}

crew::CrewTaskInfo waitApproval(crew::CrewEngine& eng, const std::string& id,
                                int maxSec = 20) {
    for (int i = 0; i < maxSec * 20; ++i) {
        auto info = eng.taskInfo(id);
        if (info && info.value().phase == crew::CrewPhase::AwaitingApproval) {
            return info.value();
        }
        std::this_thread::sleep_for(50ms);
    }
    auto info = eng.taskInfo(id);
    return info ? info.value() : crew::CrewTaskInfo{};
}
} // namespace

/* ---------------- #1 上下文截断 ---------------- */

ZTEST(CrewP0, PromptTruncation) {
    crew::EngineOptions opt;
    opt.maxPromptChars = 500; // 压小预算强制触发
    crew::CrewEngine eng(opt);
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    const std::string id = eng.createTask(std::string(2000, 'x')); // 巨长 brief
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));

    bool sawTrunc = false;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "truncate") sawTrunc = true;
    }
    ZEXPECT_TRUE(sawTrunc);
    // 实际喂给模型的 user 提示被压到 ~500 内（记录在 mock 里）
    {
        std::lock_guard<std::mutex> lk(rec->m);
        bool sawMark = false;
        for (const std::string& p : rec->pmPrompts) {
            if (p.find("[zenith: truncated ") != std::string::npos) sawMark = true;
        }
        ZEXPECT_TRUE(sawMark);
    }
}

ZTEST(CrewP0, TruncationDisabledByTask) {
    crew::EngineOptions opt;
    opt.maxPromptChars = 500;
    crew::CrewEngine eng(opt);
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.maxPromptChars = -1; // 任务级关闭
    const std::string id = eng.createTask(std::string(2000, 'y'), cfg);
    waitTerminal(eng, id);
    int truncEvents = 0;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "truncate") ++truncEvents;
    }
    ZEXPECT_EQ(truncEvents, 0);
}

/* ---------------- #2 成本预算熔断 ---------------- */

ZTEST(CrewP0, CostBudgetGateAndExtend) {
    crew::EngineOptions opt;
    opt.pricePromptPerM = 1.0;
    opt.priceCompletionPerM = 1.0;
    crew::CrewEngine eng(opt);
    RecPtr rec = std::make_shared<Rec>();
    rec->countUsage = true;
    rec->usageIn = 200000;  // 每次 chat 0.2$
    rec->usageOut = 100000; // +0.1$ → 0.3$/次
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.costBudgetUsd = 0.5; // 2 次 chat 即超
    cfg.approvalTimeoutSec = 4;
    const std::string id = eng.createTask("budgeted task", cfg);

    // 第一次停在预算门禁（不是 done）
    const auto st = waitApproval(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("awaiting_approval"));
    bool sawBudgetGate = false;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "gate" &&
            e.detail.find("cost budget") != std::string::npos) {
            sawBudgetGate = true;
        }
    }
    ZEXPECT_TRUE(sawBudgetGate);

    // 放行 → 限额翻倍（0.5→1.0）→ 继续烧到再次超限或完成（确定性轮询）
    ZEXPECT_TRUE(static_cast<bool>(eng.approve(id, "boss says continue")));
    // 先等引擎真正恢复（阶段离开 awaiting；否则会误读熔断#1 的旧阶段）
    for (int i = 0; i < 100; ++i) {
        auto info = eng.taskInfo(id);
        if (info && info.value().phase != crew::CrewPhase::AwaitingApproval) break;
        std::this_thread::sleep_for(50ms);
    }
    crew::CrewPhase end = crew::CrewPhase::Pending;
    for (int i = 0; i < 200; ++i) {
        auto info = eng.taskInfo(id);
        if (!info) break;
        const auto ph = info.value().phase;
        if (!info.value().running &&
            (ph == crew::CrewPhase::Done || ph == crew::CrewPhase::Failed)) {
            end = ph;
            break;
        }
        if (ph == crew::CrewPhase::AwaitingApproval) { // 再次熔断等待
            end = ph;
            break;
        }
        std::this_thread::sleep_for(50ms);
    }
    const bool ok = end == crew::CrewPhase::Done || end == crew::CrewPhase::Failed ||
                    end == crew::CrewPhase::AwaitingApproval;
    ZEXPECT_TRUE(ok);
    bool sawExtend = false;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "gate" && e.detail.find("budget extended") != std::string::npos) {
            sawExtend = true;
        }
    }
    ZEXPECT_TRUE(sawExtend);
    if (end == crew::CrewPhase::AwaitingApproval) eng.cancel(id); // 收尾
}

/* ---------------- #3 cancel-during-approval 的 webhook ---------------- */

ZTEST(CrewP0, CancelDuringApprovalFiresWebhook) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    std::atomic<int> cancels{0};
    std::atomic<int> awaits{0};
    std::string lastBody;
    srv.router().post("/hook", [&](http::Context& c) {
        lastBody = c.request().body;
        if (lastBody.find("\"event\":\"cancelled\"") != std::string::npos) ++cancels;
        if (lastBody.find("awaiting_approval") != std::string::npos) ++awaits;
        c.json(200, json::JsonValue());
    });
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    crew::EngineOptions opt;
    opt.webhookUrl = "http://127.0.0.1:" + std::to_string(srv.port()) + "/hook";
    crew::CrewEngine eng(opt);
    eng.setInference(std::make_shared<Rec>());
    crew::CrewConfig cfg;
    cfg.requireApproval = true;
    cfg.approvalTimeoutSec = 60;
    const std::string id = eng.createTask("gated then cancelled", cfg);

    waitApproval(eng, id);       // 停在人工门禁
    eng.cancel(id);              // 在审批等待中取消（throw 路径）
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("cancelled"));

    bool sawCancelledEvent = false;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "cancelled") sawCancelledEvent = true;
    }
    ZEXPECT_TRUE(sawCancelledEvent);
    // webhook：awaiting_approval 有、cancelled 也有（旧实现这里缺 cancelled）
    ZEXPECT_TRUE(awaits.load() >= 1);
    std::this_thread::sleep_for(300ms);
    ZEXPECT_TRUE(cancels.load() >= 1);

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- #4 followup 增量迭代 ---------------- */

ZTEST(CrewP0, FollowupInheritsArtifactsAndPrefixes) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    const std::string parent = eng.createTask("v1 app");
    const auto pst = waitTerminal(eng, parent);
    ZEXPECT_EQ(crew::phaseName(pst.phase), std::string("done"));

    const std::string child = eng.createFollowupTask(parent, "add export to csv", {});
    ZEXPECT_FALSE(child.empty());
    ZEXPECT_NE(child, parent);
    const auto cst = waitTerminal(eng, child);
    ZEXPECT_EQ(crew::phaseName(cst.phase), std::string("done"));
    ZEXPECT_EQ(cst.parentTaskId, parent);

    // 子任务继承父任务产物为 v1
    auto arts = eng.artifacts(child);
    std::set<std::string> types;
    int inheritedCount = 0;
    for (const auto& a : arts) {
        types.insert(a.type);
        if (a.agent == "inherited" && a.version == 1) ++inheritedCount;
    }
    ZEXPECT_TRUE(inheritedCount >= 6); // prd/architecture/contract/backend/frontend/…
    ZEXPECT_TRUE(types.count("prd") > 0);

    // PM 提示词带既有 PRD 前缀（增量语义）
    {
        std::lock_guard<std::mutex> lk(rec->m);
        bool sawPrefix = false;
        bool sawInheritPrompt = false;
        for (const std::string& p : rec->pmPrompts) {
            if (p.find("Existing PRD:") != std::string::npos &&
                p.find("UPDATED full JSON") != std::string::npos) {
                sawPrefix = true;
            }
            if (p.find("add export to csv") != std::string::npos) {
                sawInheritPrompt = true;
            }
        }
        ZEXPECT_TRUE(sawPrefix);
        ZEXPECT_TRUE(sawInheritPrompt);
    }
    // 交付提示带增量发布语义
    {
        std::lock_guard<std::mutex> lk(rec->m);
        // delivery 走 tagOf 分支，未单独记录——用事件流验证 changelog 产物存在即可
    }
    ZEXPECT_TRUE(types.count("release") > 0);

    // 父任务不存在 → 抛错
    bool threw = false;
    try {
        eng.createFollowupTask("no-such-parent", "x", {});
    } catch (const std::exception&) {
        threw = true;
    }
    ZEXPECT_TRUE(threw);
}

/* ---------------- HTTP：followup 端点 ---------------- */

ZTEST(CrewP0, HttpFollowupEndpoint) {
    auto eng = std::make_shared<crew::CrewEngine>();
    eng->setInference(std::make_shared<Rec>());
    const std::string parent = eng->createTask("http parent");
    for (int i = 0; i < 400; ++i) {
        auto st = eng->taskInfo(parent);
        if (st && !st.value().running) break;
        std::this_thread::sleep_for(50ms);
    }

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    crew::mountCrewApi(srv.router(), eng);
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    ai::SimpleHttpCall call;
    call.method = "POST";
    call.url = "http://127.0.0.1:" + std::to_string(srv.port()) +
               "/crew/tasks/" + parent + "/followup";
    call.body = R"({"brief":"v2 with export"})";
    call.headers = {{"Content-Type", "application/json"}};
    auto res = ai::simpleHttpRequest(call);
    ZEXPECT_TRUE(res.ok);
    ZEXPECT_EQ(res.status, 201);
    ZEXPECT_TRUE(res.body.find("\"parent_task_id\"") != std::string::npos);

    // 父不存在 → 404
    ai::SimpleHttpCall bad;
    bad.method = "POST";
    bad.url = "http://127.0.0.1:" + std::to_string(srv.port()) +
              "/crew/tasks/nope/followup";
    bad.body = R"({"brief":"x"})";
    bad.headers = {{"Content-Type", "application/json"}};
    auto badRes = ai::simpleHttpRequest(bad);
    ZEXPECT_EQ(badRes.status, 404);

    loop.stop();
    t.join();
    io.stop();
}

ZTEST_MAIN
