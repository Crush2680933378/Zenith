// M15 测试：ZenithDesk 契约 —— roster/meta/聚合 board/board 补字段/
// vote_cast/handoff/bug_assigned/test_result/story/debate 事件/CORS/WS
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include <thread>

#include "ztest.hpp"

#include "zenith/ai/provider_router.hpp"
#include "zenith/crew/crew.hpp"
#include "zenith/crew/crew_api.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/net/websocket.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
class Rec final : public ai::IInferenceService {
public:
    std::vector<ai::ModelInfo> models() override { return {}; }
    int chatSleepMs = 0;
    bool alwaysRevise = false;
    ai::ChatResult chat(const ai::ChatRequest& req,
                        const ai::TokenCallback& = nullptr,
                        const ai::TokenCallback& = nullptr) override {
        const std::string& sys = req.messages.front().content;
        const std::string& user = req.messages.back().content;
        if (chatSleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(chatSleepMs));
        ai::ChatResult r;
        r.ok = true;
        r.model = "rec";
        if (sys.find("VOTE:") != std::string::npos) {
            if (user.find("acceptance criteria") != std::string::npos) {
                r.content = (alwaysRevise || !rev_)
                    ? (rev_ = true, R"({"choice":"revise","reason":"v"})")
                    : R"({"choice":"approve","reason":"ok"})";
            } else {
                r.content = R"({"choice":"sqlite","reason":"z"})";
            }
        } else if (sys.find("ROLE: PM") != std::string::npos) {
            r.content = R"({"prd":{"summary":"d","stories":[{"id":"S1","title":"a","given":"e","when":"p","then":"c"},{"id":"S2","title":"b","given":"e","when":"v","then":"n"}]}})";
        } else if (sys.find("ROLE: ARCHITECT") != std::string::npos) {
            r.content = R"({"architecture":{"api":[{"method":"GET","path":"/a","desc":"d"}],"storage":"sqlite"}})";
        } else if (sys.find("ROLE: BACKEND") != std::string::npos ||
                   sys.find("ROLE: FRONTEND") != std::string::npos) {
            r.content = R"({"files":[{"path":"x","content":"y"}],"notes":"n"})";
        } else if (sys.find("ROLE: TESTER") != std::string::npos) {
            r.content = (user.find("RETEST") == std::string::npos && !ret_)
                ? (ret_ = true, R"({"cases":[{"id":"T1"}],"results":[{"case":"T1","status":"fail","detail":"500","component":"backend"}]})")
                : R"({"cases":[{"id":"T1"}],"results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"}]})";
        } else if (sys.find("ROLE: DEVOPS") != std::string::npos) {
            r.content = R"({"files":[{"path":"D","content":"x"}],"deploy":{"strategy":"d"}})";
        } else if (sys.find("ROLE: DELIVERY") != std::string::npos) {
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
    bool rev_ = false, ret_ = false;
};

crew::CrewTaskInfo waitTerminal(crew::CrewEngine& eng, const std::string& id,
                                int maxSec = 30) {
    for (int i = 0; i < maxSec * 20; ++i) {
        auto info = eng.taskInfo(id);
        if (info && !info.value().running) return info.value();
        std::this_thread::sleep_for(50ms);
    }
    auto info = eng.taskInfo(id);
    return info ? info.value() : crew::CrewTaskInfo{};
}
} // namespace

/* ---------------- persona 映射 ---------------- */

ZTEST(Desk, PersonaFromModel) {
    ZEXPECT_EQ(crew::personaFromModel("deepseek-chat"), std::string("deepseek"));
    ZEXPECT_EQ(crew::personaFromModel("DeepSeek-Reasoner"), std::string("deepseek"));
    ZEXPECT_EQ(crew::personaFromModel("kimi-k2-0905-preview"), std::string("kimi"));
    ZEXPECT_EQ(crew::personaFromModel("moonshot-v1-8k"), std::string("kimi"));
    ZEXPECT_EQ(crew::personaFromModel("glm-4.7"), std::string("glm"));
    ZEXPECT_EQ(crew::personaFromModel("chatglm3-6b"), std::string("glm"));
    ZEXPECT_EQ(crew::personaFromModel("mimo-7b-rl"), std::string("mimo"));
    ZEXPECT_EQ(crew::personaFromModel("MiniMax-abab6.5s-chat"), std::string("minimax"));
    ZEXPECT_EQ(crew::personaFromModel("abab5.5-chat"), std::string("minimax"));
    ZEXPECT_EQ(crew::personaFromModel("qwen3-235b"), std::string("qwen"));
    ZEXPECT_EQ(crew::personaFromModel("llama-3.3-70b"), std::string("llama"));
    ZEXPECT_EQ(crew::personaFromModel("crew-mock"), std::string(""));
}

/* ---------------- 引擎级 roster/meta ---------------- */

ZTEST(Desk, RosterWithPersonasAndTaskModels) {
    crew::EngineOptions opt;
    opt.personasJson = R"({"pm":"glm","backend":"kimi"})";
    crew::CrewEngine eng(opt);
    auto rec = std::make_shared<Rec>();
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.roleModels["architect"] = "deepseek-chat";
    const std::string id = eng.createTask("roster task", cfg);
    waitTerminal(eng, id);

    auto roster = json::JsonValue::parse(eng.roster(id).dump());
    ZEXPECT_TRUE(static_cast<bool>(roster));
    const auto* roles = roster.value()["roles"].asArray();
    ZEXPECT_EQ(roles->size(), size_t{7});
    std::map<std::string, json::JsonValue> byRole;
    for (const auto& r : *roles) byRole[r["role"].asString()] = r;
    ZEXPECT_EQ(byRole["architect"]["vote_weight"].asInt(0), int64_t{2});
    ZEXPECT_EQ(byRole["architect"]["persona"].asString(), std::string("deepseek"));
    ZEXPECT_EQ(byRole["architect"]["model"].asString(), std::string("deepseek-chat"));
    ZEXPECT_EQ(byRole["pm"]["persona"].asString(), std::string("glm"));       // 覆盖生效
    ZEXPECT_EQ(byRole["backend"]["persona"].asString(), std::string("kimi")); // 覆盖生效
    ZEXPECT_EQ(byRole["tester"]["persona"].asString(), std::string(""));      // 无模型→默认
    ZEXPECT_FALSE(byRole["pm"]["display"].asString().empty());                // 工位牌名
}

ZTEST(Desk, MetaInfoFeatures) {
    crew::CrewEngine eng;
    auto meta = json::JsonValue::parse(eng.metaInfo().dump());
    ZEXPECT_TRUE(static_cast<bool>(meta));
    ZEXPECT_EQ(meta.value()["api"].asString(), std::string("v2"));
    ZEXPECT_TRUE(meta.value()["features"]["human_voter"].asBool(false));
    ZEXPECT_TRUE(meta.value()["features"]["cancel"].asBool(false));
    ZEXPECT_FALSE(meta.value()["features"]["persistence"].asBool(true));
    ZEXPECT_TRUE(meta.value()["max_concurrent_tasks"].asInt(0) >= 1);
}

/* ---------------- 新事件：vote_cast / handoff / bug_assigned / test_result ---------------- */

ZTEST(Desk, VoteCastPerBallotEvents) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("vote cast task");
    waitTerminal(eng, id);
    int casts = 0;
    int architectWeighted = 0;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type != "vote_cast") continue;
        ++casts;
        auto d = json::JsonValue::parse(e.dataJson);
        if (e.agent == "architect" && d.value()["weight"].asInt(0) == 2) {
            ++architectWeighted;
        }
    }
    ZEXPECT_TRUE(casts >= 14);         // 2 轮投票 × 7 选民，逐票即时
    ZEXPECT_TRUE(architectWeighted >= 2);
}

ZTEST(Desk, HandoffChainEvents) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("handoff task");
    waitTerminal(eng, id);
    std::set<std::string> pairs;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type != "handoff") continue;
        auto d = json::JsonValue::parse(e.dataJson);
        pairs.insert(d.value()["from"].asString() + ">" + d.value()["to"].asString());
    }
    ZEXPECT_TRUE(pairs.count("pm>architect") > 0);
    ZEXPECT_TRUE(pairs.count("architect>backend") > 0);
    ZEXPECT_TRUE(pairs.count("architect>frontend") > 0);
    ZEXPECT_TRUE(pairs.count("backend>tester") > 0);
    ZEXPECT_TRUE(pairs.count("frontend>tester") > 0);
    ZEXPECT_TRUE(pairs.count("architect>devops") > 0);
    ZEXPECT_TRUE(pairs.count("tester>delivery") > 0);
    ZEXPECT_TRUE(pairs.count("tester>backend") > 0); // 缺陷环派单交接
}

ZTEST(Desk, BugAssignedAndTestResultStructured) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<Rec>());
    const std::string id = eng.createTask("bugs task");
    waitTerminal(eng, id);
    bool sawBug = false, sawResult = false;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "bug_assigned") {
            sawBug = true;
            auto d = json::JsonValue::parse(e.dataJson);
            ZEXPECT_EQ(d.value()["assignee"].asString(), std::string("backend"));
            const auto* issues = d.value()["issues"].asArray();
            ZEXPECT_TRUE(issues != nullptr && !issues->empty());
            ZEXPECT_EQ(issues->at(size_t{0})["case"].asString(), std::string("T1"));
        }
        if (e.type == "test_result") {
            sawResult = true;
            auto d = json::JsonValue::parse(e.dataJson);
            const int64_t total = d.value()["total"].asInt(-1);
            ZEXPECT_TRUE(total >= 1);
            ZEXPECT_TRUE(d.value()["pass_ratio"].asDouble(-1) >= 0.0);
        }
    }
    ZEXPECT_TRUE(sawBug);
    ZEXPECT_TRUE(sawResult);
}

/* ---------------- story / debate 事件 ---------------- */

ZTEST(Desk, StoryEvents) {
    crew::CrewEngine eng;
    eng.setInference(std::make_shared<Rec>());
    crew::CrewConfig cfg;
    cfg.storiesMode = true;
    const std::string id = eng.createTask("story events", cfg);
    waitTerminal(eng, id);
    std::set<std::string> started, done;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type != "story_started" && e.type != "story_done") continue;
        auto d = json::JsonValue::parse(e.dataJson);
        (e.type == "story_started" ? started : done)
            .insert(d.value()["story"].asString());
    }
    ZEXPECT_TRUE(started.count("S1") > 0);
    ZEXPECT_TRUE(started.count("S2") > 0);
    ZEXPECT_TRUE(done.count("S1") > 0);
    ZEXPECT_TRUE(done.count("S2") > 0);
}

ZTEST(Desk, DebateRoundEvent) {
    crew::CrewEngine eng;
    auto rec = std::make_shared<Rec>();
    rec->alwaysRevise = true;
    eng.setInference(rec);
    crew::CrewConfig cfg;
    cfg.prdVoteRounds = 2;
    const std::string id = eng.createTask("debate events", cfg);
    waitTerminal(eng, id);
    int rounds = 0;
    for (const crew::CrewEvent& e : eng.events(id)) {
        if (e.type == "debate_round") {
            ++rounds;
            auto d = json::JsonValue::parse(e.dataJson);
            ZEXPECT_TRUE(d.value()["budget"].asInt(0) == 2);
        }
    }
    ZEXPECT_EQ(rounds, 2);
}

/* ---------------- HTTP 层：roster/meta/聚合 board/CORS ---------------- */

namespace {
struct Http {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    // 声明顺序 = 析构反序：srv 先于 eng 销毁（WS 会话回调不再触达死引擎）
    crew::CrewEngine eng;
    std::thread t;
    http::HttpServer srv{&loop, &io};
    std::string baseUrl;

    explicit Http(crew::EngineOptions opt = {}) : eng(std::move(opt)) {}
    void start(const std::string& token = {}) {
        io.start();
        eng.setInference(std::make_shared<Rec>());
        crew::mountCrewApi(srv.router(),
                           std::shared_ptr<crew::CrewEngine>(std::shared_ptr<void>(),
                                                             &eng),
                           token);
        crew::mountCrewWs(srv, std::shared_ptr<crew::CrewEngine>(std::shared_ptr<void>(),
                                                                 &eng),
                          token);
        (void)srv.listen("127.0.0.1", 0);
        baseUrl = "http://127.0.0.1:" + std::to_string(srv.port());
        t = std::thread([&] { loop.loop(); });
        std::this_thread::sleep_for(200ms);
    }
    ~Http() {
        loop.stop();
        if (t.joinable()) t.join();
        io.stop();
    }
    ai::SimpleHttpResult get(const std::string& path) {
        ai::SimpleHttpCall call;
        call.method = "GET";
        call.url = baseUrl + path;
        return ai::simpleHttpRequest(call);
    }
};
} // namespace

ZTEST(Desk, HttpRosterMetaAndCors) {
    Http h;
    h.start();

    auto roster = h.get("/crew/roster");
    ZEXPECT_TRUE(roster.ok);
    ZEXPECT_TRUE(roster.body.find("\"architect\"") != std::string::npos);
    ZEXPECT_TRUE(roster.body.find("\"vote_weight\"") != std::string::npos);

    auto meta = h.get("/crew/meta");
    ZEXPECT_TRUE(meta.ok);
    ZEXPECT_TRUE(meta.body.find("\"features\"") != std::string::npos);

    // CORS 预检：OPTIONS + Origin → 204 + Access-Control-Allow-Origin
    ai::SimpleHttpCall pre;
    pre.method = "OPTIONS";
    pre.url = h.baseUrl + "/crew/tasks";
    pre.headers = {{"Origin", "http://localhost:5173"},
                   {"Access-Control-Request-Method", "POST"}};
    auto preRes = ai::simpleHttpRequest(pre);
    ZEXPECT_EQ(preRes.status, 204);
    ZEXPECT_TRUE(preRes.body.empty() || preRes.body.find("204") == std::string::npos);
}

ZTEST(Desk, HttpAggregateBoardAndEnrichedFields) {
    Http h;
    h.start();
    crew::CrewConfig cfg;
    cfg.roleModels["pm"] = "glm-4.7";
    auto slow = std::make_shared<Rec>();
    slow->chatSleepMs = 60;
    h.eng.setInference(slow);
    const std::string id1 = h.eng.createTask("agg one", cfg);
    const std::string id2 = h.eng.createTask("agg two");
    std::this_thread::sleep_for(300ms); // 两个都在跑

    auto agg = h.get("/crew/board");
    ZEXPECT_TRUE(agg.ok);
    auto v = json::JsonValue::parse(agg.body);
    const auto* tasks = v.value()["tasks"].asArray();
    ZEXPECT_TRUE(tasks != nullptr && tasks->size() >= 2);
    bool sawPersona = false, sawIdle = false, sawModel = false;
    for (const auto& t : *tasks) {
        const auto* agents = t["agents"].asArray();
        ZEXPECT_TRUE(agents != nullptr && agents->size() == 7);
        for (const auto& a : *agents) {
            if (!a["persona"].isNull()) sawPersona = true;
            if (a["idle_ms"].asInt(-1) >= 0) sawIdle = true;
            if (!a["model"].isNull()) sawModel = true;
        }
    }
    ZEXPECT_TRUE(sawPersona);
    ZEXPECT_TRUE(sawIdle);
    ZEXPECT_TRUE(sawModel);

    // 单任务 board：config + pm 的 model/persona 正确解析
    auto board = h.get("/crew/tasks/" + id1 + "/board");
    ZEXPECT_TRUE(board.ok);
    auto b = json::JsonValue::parse(board.body);
    ZEXPECT_TRUE(b.value()["config"].isObject());
    const auto* agents = b.value()["agents"].asArray();
    for (const auto& a : *agents) {
        if (a["role"].asString() == "pm") {
            ZEXPECT_EQ(a["model"].asString(), std::string("glm-4.7"));
            ZEXPECT_EQ(a["persona"].asString(), std::string("glm"));
        }
    }
    waitTerminal(h.eng, id1);
    waitTerminal(h.eng, id2);
}

/* ---------------- WebSocket 端点 ---------------- */

ZTEST(Desk, WebSocketWatchAndEvents) {
    Http h;
    h.start();
    const std::string id = h.eng.createTask("ws task");
    std::this_thread::sleep_for(100ms);

    net::EventLoop cloop;
    std::thread ct([&] { cloop.loop(); });
    std::atomic<bool> done{false};
    std::atomic<int> frames{0};
    std::atomic<bool> gotHello{false};
    std::atomic<bool> gotCrewEvent{false};
    std::atomic<bool> gotPong{false};

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = h.srv.port();
    opt.path = "/crew/ws";
    opt.timeoutMs = 5000;
    client->connect(
        &cloop, opt,
        [&](const net::WebSocketSession::Ptr& s0) {
            auto s = s0;
            s->setMessageCallback(
                [&](const net::WebSocketSession::Ptr&, std::string m, bool) {
                    const int n = ++frames;
                    auto v = json::JsonValue::parse(m);
                    const std::string type =
                        v ? v.value()["type"].asString() : std::string();
                    if (type == "hello") gotHello.store(true);
                    if (type == "phase" || type == "agent_start" || type == "vote_cast" ||
                        type == "artifact") {
                        gotCrewEvent.store(true);
                    }
                    if (type == "pong") gotPong.store(true);
                    if (n == 3) s->sendText(R"({"type":"ping"})");
                    if (gotCrewEvent.load() && n > 5) {
                        s->close();
                        done.store(true);
                    }
                });
            s->setCloseCallback([&](const net::WebSocketSession::Ptr&) {
                done.store(true);
            });
            s->sendText(R"({"type":"watch","task":")" + id + R"("})");
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "ws connect failed: " + reason);
        });

    for (int i = 0; i < 800 && !done.load(); ++i) std::this_thread::sleep_for(10ms);
    cloop.stop();
    if (ct.joinable()) ct.join();
    ZEXPECT_TRUE(gotHello.load());
    ZEXPECT_TRUE(gotCrewEvent.load());
    ZEXPECT_TRUE(frames.load() > 3);

    waitTerminal(h.eng, id);
}

ZTEST_MAIN
