// M14 测试：ai_crew 全面强化 —— 取消/每角色模型/结构化重试/成本/流式 token/
// 评分卡/持久化/断点恢复/组织记忆/严格重测/真并行/人工投票/辩论/真执行/
// story 子环/webhook/OTel/角色配置/导出/写鉴权
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "ztest.hpp"

#include "zenith/ai/provider_router.hpp"
#include "zenith/crew/crew.hpp"
#include "zenith/crew/crew_api.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/otel.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- 可编程剧本推理 ---------------- */

namespace {
class Rec final : public ai::IInferenceService {
public:
    std::vector<ai::ModelInfo> models() override { return {}; }

    std::mutex m;
    std::map<std::string, std::vector<std::string>> prompts; // role → user prompts
    std::map<std::string, std::vector<std::string>> models_; // role → req.model
    std::atomic<int> chatCount{0};
    std::atomic<int> implInFlight{0}, implMaxInFlight{0};
    int chatSleepMs = 0;
    // 行为开关
    int pmBadJsonTimes = 0;        // PM 前 N 次回非法 JSON
    bool voteAlwaysRevise = false; // PRD 评审全员 revise（辩论测试）
    bool streamDeltas = false;     // 回调 onToken
    int64_t usageIn = 0, usageOut = 0;
    std::string deliveryMarker = "ROLE: DELIVERY";
    std::vector<std::pair<std::string, std::string>> extraTesterCommands; // exec 验证

    ai::ChatResult chat(const ai::ChatRequest& req,
                        const ai::TokenCallback& onToken = nullptr,
                        const ai::TokenCallback& = nullptr) override {
        const std::string& sys = req.messages.front().content;
        const std::string& user = req.messages.back().content;
        const std::string role = tagOf(sys);
        {
            std::lock_guard<std::mutex> lk(m);
            prompts[role].push_back(user);
            models_[role].push_back(req.model);
        }
        ++chatCount;
        const bool impl = role == "backend" || role == "frontend";
        // 入口即计数（睡眠前）——并发重叠测量与线程启动抖动解耦
        if (impl) {
            const int n = ++implInFlight;
            int cur = implMaxInFlight.load();
            while (n > cur && !implMaxInFlight.compare_exchange_weak(cur, n)) {}
        }
        if (chatSleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(chatSleepMs));
        if (streamDeltas && onToken) {
            onToken("abcdefghijklmnopqrstuvwxyz012345"); // 32 字符 → 触发节流事件
        }

        ai::ChatResult r;
        r.ok = true;
        r.model = "rec";
        r.usage.promptTokens = static_cast<int>(usageIn);
        r.usage.completionTokens = static_cast<int>(usageOut);

        if (role == "vote") {
            if (user.find("acceptance criteria") != std::string::npos) {
                const bool revise = voteAlwaysRevise || !revoted_;
                revoted_ = true;
                r.content = revise ? R"({"choice":"revise","reason":"vague"})"
                                   : R"({"choice":"approve","reason":"ok"})";
            } else {
                r.content = R"({"choice":"sqlite","reason":"zero-ops"})";
            }
        } else if (role == "pm") {
            if (pmBadJsonTimes > 0) {
                --pmBadJsonTimes;
                r.content = "sorry this is not json at all";
            } else {
                r.content = R"({"prd":{"summary":"demo","stories":[)"
                            R"({"id":"S1","title":"create","given":"e","when":"p","then":"c"},)"
                            R"({"id":"S2","title":"cast","given":"e","when":"v","then":"n"}]}})";
            }
        } else if (role == "architect") {
            r.content = R"({"architecture":{"style":"rest","storage":"sqlite",)"
                        R"("api":[{"method":"GET","path":"/api/items","desc":"list"}],)"
                        R"("notes":"n"}})";
        } else if (role == "backend" || role == "frontend") {
            r.content = R"({"files":[{"path":"src/a.cpp","content":"// impl"}],"notes":"d"})";
        } else if (role == "tester") {
            if (user.find("STORY ") != std::string::npos) {
                r.content = R"({"cases":[{"id":"T1"}],"results":[)"
                            R"({"case":"T1","status":"pass","detail":"ok","component":"backend"}]})";
            } else if (user.find("RETEST") == std::string::npos && !retested_) {
                retested_ = true;
                r.content = R"({"cases":[{"id":"T1"}],"results":[)"
                            R"({"case":"T1","status":"fail","detail":"500","component":"backend"}]})";
            } else {
                json::JsonValue j = json::JsonValue::parse(
                    R"({"cases":[{"id":"T1"}],"results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"}]})").value();
                if (!extraTesterCommands.empty()) {
                    json::JsonValue cmds = json::JsonValue::array();
                    for (auto& [c, expect] : extraTesterCommands) cmds.push_back(c);
                    j["commands"] = std::move(cmds);
                }
                r.content = j.dump();
            }
        } else if (role == "devops") {
            r.content = R"({"files":[{"path":"Dockerfile","content":"FROM x"}],"deploy":{"strategy":"docker"}})";
        } else if (sys.find(deliveryMarker) != std::string::npos) {
            r.content = R"({"changelog":"# v0.1.0","release_notes":"n","version":"0.1.0"})";
        } else {
            r.content = "{}";
        }
        if (impl) --implInFlight;
        return r;
    }
    ai::EmbeddingResult embeddings(const std::string&, const std::string&) override {
        ai::EmbeddingResult e;
        e.ok = false;
        return e;
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
    bool revoted_ = false;
    bool retested_ = false;
};

using RecPtr = std::shared_ptr<Rec>;

crew::CrewTaskInfo waitTerminal(crew::CrewEngine& eng, const std::string& id, int maxSec = 30) {
    for (int i = 0; i < maxSec * 20; ++i) {
        auto info = eng.taskInfo(id);
        if (info && !info.value().running) return info.value();
        std::this_thread::sleep_for(50ms);
    }
    auto info = eng.taskInfo(id);
    return info ? info.value() : crew::CrewTaskInfo{};
}

bool hasEvent(const crew::CrewEngine& eng, const std::string& id, const std::string& type,
              std::string* detailOut = nullptr) {
    for (const crew::CrewEvent& e : const_cast<crew::CrewEngine&>(eng).events(id)) {
        if (e.type == type) {
            if (detailOut != nullptr) *detailOut = e.detail;
            return true;
        }
    }
    return false;
}
} // namespace

/* ---------------- P0 ---------------- */

ZTEST(CrewM14, CancelMidRun) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->chatSleepMs = 120;
    eng.setInference(rec);
    const std::string id = eng.createTask("slow task");
    std::this_thread::sleep_for(400ms);
    ZEXPECT_TRUE(static_cast<bool>(eng.cancel(id)));
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("cancelled"));
    ZEXPECT_FALSE(st.error.empty());
    ZEXPECT_TRUE(hasEvent(eng, id, "cancelled"));
    // 已终止再取消 → 明确报错
    ZEXPECT_FALSE(static_cast<bool>(eng.cancel(id)));
}

ZTEST(CrewM14, PerRoleModelRouting) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.roleModels["pm"] = "pm-cheap";
    cfg.roleModels["architect"] = "arch-strong";
    const std::string id = eng.createTask("models per role", cfg);
    waitTerminal(eng, id);
    {
        std::lock_guard<std::mutex> lk(rec->m);
        for (const std::string& mm : rec->models_["pm"]) ZEXPECT_EQ(mm, std::string("pm-cheap"));
        for (const std::string& mm : rec->models_["architect"]) {
            ZEXPECT_EQ(mm, std::string("arch-strong"));
        }
        for (const std::string& mm : rec->models_["tester"]) ZEXPECT_EQ(mm, std::string(""));
    }
}

ZTEST(CrewM14, StructuredOutputRetry) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->pmBadJsonTimes = 1; // 第一次坏 JSON → 重试修正
    eng.setInference(rec);
    const std::string id = eng.createTask("retry json");
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    ZEXPECT_TRUE(hasEvent(eng, id, "retry"));
    {
        std::lock_guard<std::mutex> lk(rec->m);
        ZEXPECT_TRUE(rec->prompts["pm"].size() >= 2); // PM 至少两次（坏+好）
        // 重试提示带错误反馈
        bool sawFeedback = false;
        for (const std::string& p : rec->prompts["pm"]) {
            if (p.find("INVALID JSON") != std::string::npos) sawFeedback = true;
        }
        ZEXPECT_TRUE(sawFeedback);
    }
}

ZTEST(CrewM14, CostAndTokenAccounting) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->usageIn = 1000;
    rec->usageOut = 500;
    eng.setInference(rec);
    crew::EngineOptions opt;
    opt.pricePromptPerM = 0.5;
    opt.priceCompletionPerM = 2.0;
    eng.setOptions(opt);
    const std::string id = eng.createTask("costed task");
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    ZEXPECT_TRUE(st.tokensIn >= 1000);
    ZEXPECT_TRUE(st.tokensOut >= 500);
    const double perCall = 1000 / 1e6 * 0.5 + 500 / 1e6 * 2.0;
    ZEXPECT_TRUE(st.costUsd > perCall * 3 && st.costUsd < perCall * 100);
    ZEXPECT_TRUE(st.roleUsage.count("pm") > 0);
    ZEXPECT_TRUE(st.roleUsage.count("tester") > 0);
}

ZTEST(CrewM14, StreamingTokensIntoEvents) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->streamDeltas = true;
    eng.setInference(rec);
    const std::string id = eng.createTask("stream task");
    waitTerminal(eng, id);
    int tokenEvents = 0;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "token") ++tokenEvents;
    }
    ZEXPECT_TRUE(tokenEvents >= 5); // 多角色都有增量
}

ZTEST(CrewM14, QualityScorecard) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("scored task");
    const auto stQ = waitTerminal(eng, id);
    std::string cardText;
    for (const crew::Artifact& a : eng.artifacts(id)) {
        if (a.type == "scorecard") cardText = a.content;
    }
    ZEXPECT_FALSE(cardText.empty());
    auto card = json::JsonValue::parse(cardText);
    std::fprintf(stderr, "DBG-CARD=[%s]
", cardText.c_str());
    ZEXPECT_TRUE(static_cast<bool>(card));
    ZEXPECT_TRUE(card.value()["test_cases"].asInt(0) >= 1);
    ZEXPECT_TRUE(card.value()["score"].asInt(0) >= 90);
    ZEXPECT_TRUE(card.value()["docs_present"].asBool(false));
}

/* ---------------- P1 ---------------- */

ZTEST(CrewM14, StrictRetestPrompt) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    const std::string id = eng.createTask("strict retest");
    waitTerminal(eng, id);
    std::lock_guard<std::mutex> lk(rec->m);
    bool sawStrict = false;
    for (const std::string& p : rec->prompts["tester"]) {
        if (p.find("RETEST") != std::string::npos &&
            p.find("verify") != std::string::npos) {
            sawStrict = true; // 严格模式：要求对照修复证据验证，不是直接改口
        }
    }
    ZEXPECT_TRUE(sawStrict);
}

ZTEST(CrewM14, ParallelImplementOverlap) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->chatSleepMs = 300; // 慢机器（CI）下也保证两线程窗口重叠
    eng.setInference(rec);
    const std::string id = eng.createTask("parallel impl");
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    ZEXPECT_TRUE(rec->implMaxInFlight.load() >= 2); // 真·并行
}

ZTEST(CrewM14, PersistenceAndResume) {
    const std::string dir = "zcrew14.tmp";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    std::string id;

    // 引擎 A：跑到审批门禁（running）后被"重启"
    {
        RecPtr rec = std::make_shared<Rec>();
        rec->chatSleepMs = 20;
        crew::EngineOptions opt;
        opt.persistDir = dir;
        crew::CrewEngine eng(opt);
        eng.setInference(rec);
        crew::CrewConfig cfg;
        cfg.requireApproval = true;
        id = eng.createTask("persisted task", cfg);
        for (int i = 0; i < 400; ++i) {
            auto st = eng.taskInfo(id);
            if (st && st.value().phase == crew::CrewPhase::AwaitingApproval) break;
            std::this_thread::sleep_for(50ms);
        }
    } // 析构 = 模拟重启

    // 引擎 B：同目录恢复 → resumable → 放行 + 断点续跑
    RecPtr rec2 = std::make_shared<Rec>();
    crew::EngineOptions opt2;
    opt2.persistDir = dir;
    crew::CrewEngine eng2(opt2);
    eng2.setInference(rec2);
    auto listed = eng2.listTasks();
    ZEXPECT_EQ(listed.size(), size_t{1});
    ZEXPECT_EQ(listed[0].id, id);
    ZEXPECT_TRUE(listed[0].resumable);
    ZEXPECT_TRUE(eng2.artifacts(id).size() >= 3); // prd/architecture/contract 已落盘

    ZEXPECT_TRUE(static_cast<bool>(eng2.approve(id, "looking good")));
    ZEXPECT_TRUE(static_cast<bool>(eng2.resume(id)));
    const auto st = waitTerminal(eng2, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    // resume 复用已有产物 → 新引擎的调用量显著小于全量重跑（全量 > 25）
    ZEXPECT_TRUE(rec2->chatCount.load() < 25);
    // 审批评论进了事件流
    bool sawComment = false;
    for (const crew::CrewEvent& e : eng2.events(id)) {
        if (e.type == "approval" && e.detail.find("looking good") != std::string::npos) {
            sawComment = true;
        }
    }
    ZEXPECT_TRUE(sawComment);

    std::filesystem::remove_all(dir, ec);
}

ZTEST(CrewM14, OtelSpansExported) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    std::string lastBody;
    srv.router().post("/v1/traces", [&](http::Context& c) {
        lastBody = c.request().body;
        c.json(200, json::JsonValue());
    });
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    obs::OtelConfig oc;
    oc.endpoint = "http://127.0.0.1:" + std::to_string(srv.port()) + "/v1/traces";
    oc.flushIntervalMs = 0;
    auto exporter = std::make_shared<obs::OtelExporter>(oc);

    crew::EngineOptions opt;
    opt.otel = exporter;
    crew::CrewEngine eng(opt);
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("traced task");
    waitTerminal(eng, id);
    exporter->flushNow();
    ZEXPECT_TRUE(lastBody.find("crew.pm") != std::string::npos);
    ZEXPECT_TRUE(lastBody.find("crew.tester") != std::string::npos);

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- P2 ---------------- */

ZTEST(CrewM14, OrganizationalMemory) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    const std::string id1 = eng.createTask("team voting app for polls");
    waitTerminal(eng, id1);
    // 任务完工归档后，同类需求开工要能检索到经验注入 PM 提示
    const std::string id2 = eng.createTask("team voting app for surveys");
    waitTerminal(eng, id2);
    std::lock_guard<std::mutex> lk(rec->m);
    bool sawLessons = false;
    for (const std::string& p : rec->prompts["pm"]) {
        if (p.find("Past project lessons") != std::string::npos) sawLessons = true;
    }
    ZEXPECT_TRUE(sawLessons);
    // lk 持有期间不再碰引擎
}

ZTEST(CrewM14, HumanVoterBallot) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->chatSleepMs = 50; // 投票慢一点，保证人工票窗口可命中
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.humanVoter = true;
    cfg.humanVoteTimeoutSec = 1;
    const std::string id = eng.createTask("human vote task", cfg);
    std::this_thread::sleep_for(600ms); // 7 角色 x50ms 投完 → 人工票窗口内
    ZEXPECT_TRUE(static_cast<bool>(eng.castHumanVote(id, "revise")));
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    bool sawHuman = false; // 第一张人工票 = PRD 评审（后续票默认首项也合法）
    for (const crew::VoteRecord& v : eng.votes(id)) {
        for (const auto& [agent, choice] : v.votes) {
            if (agent == "human" && !sawHuman) {
                sawHuman = true;
                ZEXPECT_EQ(choice, std::string("revise"));
            }
        }
    }
    ZEXPECT_TRUE(sawHuman);
}

ZTEST(CrewM14, DebateRounds) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->voteAlwaysRevise = true; // 全员永远 revise → 辩论到轮数上限
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.prdVoteRounds = 2;
    const std::string id = eng.createTask("debate task", cfg);
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    int prdVersion = 0;
    for (const crew::Artifact& a : eng.artifacts(id)) {
        if (a.type == "prd") prdVersion = std::max(prdVersion, a.version);
    }
    ZEXPECT_EQ(prdVersion, 3); // 初稿 + 2 轮 revise
    ZEXPECT_TRUE(hasEvent(eng, id, "gate"));
    ZEXPECT_TRUE(eng.votes(id).size() >= 3); // PRD×2 + storage×1
}

ZTEST(CrewM14, ExecVerifyRunsCommands) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    rec->extraTesterCommands = {{"echo crew-ok-9d21", ""}};
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.execVerify = true;
    const std::string id = eng.createTask("exec verify", cfg);
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    std::string detail;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "exec") std::fprintf(stderr, "DBG-EXEC=[%s]
", e.detail.c_str());
    }
    ZEXPECT_TRUE(hasEvent(eng, id, "exec", &detail));
    std::fprintf(stderr, "DBG-FIRST-EXEC=[%s]
", detail.c_str());
    ZEXPECT_TRUE(detail.find("crew-ok-9d21") != std::string::npos);
    ZEXPECT_TRUE(detail.find("pass") != std::string::npos);
}

ZTEST(CrewM14, StoriesModeParallelSlices) {
    crew::CrewEngine eng;
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.storiesMode = true;
    const std::string id = eng.createTask("stories task", cfg);
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    std::set<std::string> types;
    for (const crew::Artifact& a : eng.artifacts(id)) {
        types.insert(a.type);
    }
    ZEXPECT_TRUE(types.count("story-S1-backend") > 0);
    ZEXPECT_TRUE(types.count("story-S2-backend") > 0);
    ZEXPECT_TRUE(types.count("story-S1-tests") > 0);
    // story 提示确实带 STORY 标记
    std::lock_guard<std::mutex> lk(rec->m);
    bool sawStory = false;
    for (const std::string& p : rec->prompts["backend"]) {
        if (p.find("STORY S2") != std::string::npos) sawStory = true;
    }
    ZEXPECT_TRUE(sawStory);
}

ZTEST(CrewM14, RolesConfigOverride) {
    crew::EngineOptions opt;
    // 复用默认 7 角色但替换交付人设（voteWeight 保持默认）
    opt.rolesJson = R"([
      {"role":"pm","prompt":"ROLE: PM. produce PRD JSON.","voteWeight":1},
      {"role":"architect","prompt":"ROLE: ARCHITECT. produce architecture JSON.","voteWeight":2},
      {"role":"backend","prompt":"ROLE: BACKEND. produce files JSON."},
      {"role":"frontend","prompt":"ROLE: FRONTEND. produce files JSON."},
      {"role":"tester","prompt":"ROLE: TESTER. produce cases/results JSON."},
      {"role":"devops","prompt":"ROLE: DEVOPS. produce files JSON."},
      {"role":"delivery","prompt":"ROLE: DELIVERY V2. produce release JSON."}
    ])";
    crew::CrewEngine eng(opt);
    RecPtr rec = std::make_shared<Rec>();
    eng.setInference(rec);
    const std::string id = eng.createTask("custom roles");
    const auto st = waitTerminal(eng, id);
    ZEXPECT_EQ(crew::phaseName(st.phase), std::string("done"));
    ZEXPECT_EQ(st.agents.size(), size_t{7});
    ZEXPECT_FALSE(rec->models_["delivery"].empty()); // V2 人设被命中（走默认分支会返回 {}）
    std::string release;
    for (const crew::Artifact& a : eng.artifacts(id)) {
        if (a.type == "release") release = a.content;
    }
    ZEXPECT_TRUE(release.find("changelog") != std::string::npos);
}

ZTEST(CrewM14, WebhookFiredOnDone) {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    std::string lastBody;
    srv.router().post("/hook", [&](http::Context& c) {
        lastBody = c.request().body;
        c.json(200, json::JsonValue());
    });
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    crew::EngineOptions opt;
    opt.webhookUrl = "http://127.0.0.1:" + std::to_string(srv.port()) + "/hook";
    crew::CrewEngine eng(opt);
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("hooked task");
    waitTerminal(eng, id);
    ZEXPECT_TRUE(lastBody.find("\"event\":\"done\"") != std::string::npos);
    ZEXPECT_TRUE(lastBody.find(id) != std::string::npos);

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- HTTP 层（导出 / 写鉴权） ---------------- */

ZTEST(CrewM14, HttpExportAndTokenGuard) {
    crew::EngineOptions opt;
    opt.approvalToken = "s3cret";
    auto eng = std::make_shared<crew::CrewEngine>(opt);
    eng->setInference(std::make_shared<Rec>());
    const std::string id = eng->createTask("http task");
    waitTerminal(*eng, id);

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer srv(&loop, &io);
    crew::mountCrewApi(srv.router(), eng, "s3cret");
    (void)srv.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(200ms);

    auto post = [&](const std::string& path, const std::string& token) {
        ai::SimpleHttpCall call;
        call.method = "POST";
        call.url = "http://127.0.0.1:" + std::to_string(srv.port()) + path;
        if (!token.empty()) call.headers.emplace_back("X-Crew-Token", token);
        return ai::simpleHttpRequest(call);
    };

    // 导出包：task + artifacts + votes + events
    ai::SimpleHttpCall get;
    get.method = "GET";
    get.url = "http://127.0.0.1:" + std::to_string(srv.port()) + "/crew/tasks/" + id + "/export";
    auto ex = ai::simpleHttpRequest(get);
    ZEXPECT_TRUE(ex.ok);
    ZEXPECT_TRUE(ex.body.find("\"prd\"") != std::string::npos);
    ZEXPECT_TRUE(ex.body.find("\"events\"") != std::string::npos);

    // 写鉴权：无 token 401 / 错 token 401 / 对 token 200
    ZEXPECT_EQ(post("/crew/tasks/" + id + "/approve", "").status, 401);
    ZEXPECT_EQ(post("/crew/tasks/" + id + "/approve", "wrong").status, 401);
    ZEXPECT_EQ(post("/crew/tasks/" + id + "/approve", "s3cret").status, 200);
    // 非 running 取消 → 409（带 token 通过鉴权后业务拒绝）
    ZEXPECT_EQ(post("/crew/tasks/" + id + "/cancel", "s3cret").status, 409);

    loop.stop();
    t.join();
    io.stop();
}

ZTEST_MAIN
