// ============================================================================
//  09_crew —— 多 Agent 软件开发集群示例（M11 / SDLC-Crew）
//  一句话开任务，7 个 Agent（PM/架构师/后端/前端/测试/运维/交付）自动协作：
//  投票决策 → 产物库 → 缺陷环 → 发布；全程事件流可观测。
//
//  本示例用 mock 推理演示全流程（生产：ai_inference + ai_llama/ai_ninfer/远端
//  OpenAI API 提供真实推理，ai_crew 插件自动经 IInferenceService 接入）。
//
//  端点：
//    GET  /crew/dashboard          内嵌仪表盘（M13.4：列表/角色卡/SSE/产物/投票/审批）
//    POST /crew/tasks            {"brief":"团队投票应用"}   ← 用户唯一入口
//    GET  /crew/tasks/:id/board  状态板（前端仪表盘就绪）
//    GET  /crew/tasks/:id/events 事件流（增量轮询）
//    GET  /crew/tasks/:id/events/stream  SSE 实时流
//    GET  /crew/tasks/:id/artifacts | votes
//    POST /crew/tasks/:id/approve
// ============================================================================
#include <memory>
#include <thread>

#include "zenith/crew/crew.hpp"
#include "zenith/crew/crew_api.hpp"
#include "zenith/crew/crew_dashboard.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/net/signal.hpp"

// ---- mock 推理（演示剧本；生产换 ai_llama / OpenAI 上游）----
class MockCrewInference final : public zenith::ai::IInferenceService {
public:
    std::vector<zenith::ai::ModelInfo> models() override {
        zenith::ai::ModelInfo m;
        m.id = "crew-mock";
        return {m};
    }
    zenith::ai::ChatResult chat(const zenith::ai::ChatRequest& req,
                                const zenith::ai::TokenCallback& = nullptr,
                                const zenith::ai::TokenCallback& = nullptr) override {
        using namespace zenith;
        const std::string& sys = req.messages.front().content;
        ai::ChatResult r;
        r.ok = true;
        r.model = "crew-mock";
        if (sys.find("VOTE:") != std::string::npos) {
            r.content = sys.find("storage") != std::string::npos
                            ? R"({"choice":"sqlite","reason":"zero ops for demo"})"
                            : R"({"choice":"approve","reason":"ok"})";
        } else if (sys.find("ROLE: PM") != std::string::npos) {
            r.content = R"({"prd":{"summary":"polling app","stories":[
                {"id":"S1","title":"create poll","given":"app open","when":"create","then":"poll listed"},
                {"id":"S2","title":"cast vote","given":"poll exists","when":"vote","then":"count+1"}]}})";
        } else if (sys.find("ROLE: ARCHITECT") != std::string::npos) {
            r.content = R"({"architecture":{"style":"rest","storage":"sqlite",
                "api":[{"method":"POST","path":"/api/polls","desc":"create"},
                       {"method":"POST","path":"/api/polls/:id/votes","desc":"vote"},
                       {"method":"GET","path":"/api/polls/:id","desc":"live result"}],
                "notes":"polling over REST + WS push"}})";
        } else if (sys.find("ROLE: BACKEND") != std::string::npos) {
            r.content = R"({"files":[{"path":"src/polls.cpp","content":"// 3 endpoints"}],"notes":"done"})";
        } else if (sys.find("ROLE: FRONTEND") != std::string::npos) {
            r.content = R"({"files":[{"path":"web/app.html","content":"<!-- poll UI -->"}],"notes":"done"})";
        } else if (sys.find("ROLE: TESTER") != std::string::npos) {
            const bool retest = req.messages.back().content.find("RETEST") !=
                                std::string::npos;
            r.content = retest
                ? R"({"cases":[{"id":"T1","story":"S1"}],"results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"}]})"
                : R"({"cases":[{"id":"T1","story":"S1"},{"id":"T2","story":"S2"}],
                     "results":[{"case":"T1","status":"pass","detail":"ok","component":"backend"},
                                {"case":"T2","status":"fail","detail":"race on count","component":"backend"}]})";
        } else if (sys.find("ROLE: DEVOPS") != std::string::npos) {
            r.content = R"({"files":[{"path":"Dockerfile","content":"FROM zenith"}],"deploy":{"strategy":"docker","smoke":"GET /api/polls/x"}})";
        } else if (sys.find("ROLE: DELIVERY") != std::string::npos) {
            r.content = R"({"changelog":"# v0.1.0 poll app","release_notes":"crew-built","version":"0.1.0"})";
        } else {
            r.content = "{}";
        }
        return r;
    }
    zenith::ai::EmbeddingResult embeddings(const std::string&,
                                           const std::string&) override {
        zenith::ai::EmbeddingResult e;
        e.ok = false;
        return e;
    }
    std::string backend() const override { return "crew-mock"; }
};

int main() {
    using namespace zenith;
    auto& facade = log::LoggerFacade::instance();
    facade.setLevel(log::LogLevel::Info);
    facade.addSink(std::make_shared<log::ConsoleLogger>(true, true));

    net::EventLoop loop;
    net::EventLoopThreadPool io(2);
    io.start();
    http::HttpServer server(&loop, &io);

    // crew 引擎直挂（插件形态：ai_crew.dll 注册 ICrewService，宿主同款挂载）
    auto engine = std::make_shared<crew::CrewEngine>();
    engine->setInference(std::make_shared<MockCrewInference>());
    crew::mountCrewApi(server.router(), engine);
    crew::mountCrewDashboard(server.router());

    if (!server.listen("0.0.0.0", 8093)) return 1;
    ZLOG_INFO("Zenith Crew demo on http://127.0.0.1:8093");
    ZLOG_INFO("  浏览器打开 http://127.0.0.1:8093/crew/dashboard  ← 内嵌仪表盘");
    ZLOG_INFO("  curl -X POST localhost:8093/crew/tasks -d '{\"brief\":\"team polling app\"}' -H 'Content-Type: application/json'");
    ZLOG_INFO("  curl localhost:8093/crew/tasks/<id>/board");
    ZLOG_INFO("  curl -N localhost:8093/crew/tasks/<id>/events/stream   # SSE 实时");

    net::installSignalHandler([&] {
        server.beginGracefulShutdown([&] { loop.stop(); });
    });
    loop.loop();
    io.stop();
    facade.shutdown();
    return 0;
}
