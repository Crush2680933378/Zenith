// ============================================================================
//  08_ai_agent —— AI Agent 后端全链路示例（M10）
//   组合：推理(IInferenceService) + 编排(zenith::agent) +
//         工具协议(zenith::mcp) + 记忆检索(zenith::vector/HNSW)
//
//   本示例以「进程内组装」演示（mock 推理后端——真模型经 ai_llama/ai_ninfer
//   插件注入即可替换）；插件形态部署见 build/plugins/*.dll：
//     ai_inference + ai_llama + db_vector + ai_mcp + ai_agent
//
//   端点：
//     POST /agent/run     {"message":"...","session":"s1"}
//     POST /chat          OpenAI 兼容直通（无编排）
//     POST /collections/*  Qdrant 兼容向量 API（RAG 记忆）
//     POST /mcp           MCP JSON-RPC（Agent 工具入口）
// ============================================================================
#include <memory>
#include <thread>

#include "zenith/agent/agent.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/mcp/mcp_api.hpp"
#include "zenith/net/signal.hpp"
#include "zenith/vector/vector_api.hpp"

// ---- mock 推理后端：演示用（生产换 ai_llama / ai_ninfer 插件注册的服务）----
class MockInference final : public zenith::ai::IInferenceService {
public:
    std::vector<zenith::ai::ModelInfo> models() override {
        zenith::ai::ModelInfo m;
        m.id = "zenith-mock";
        return {m};
    }
    zenith::ai::ChatResult chat(const zenith::ai::ChatRequest& req,
                                const zenith::ai::TokenCallback& = nullptr,
                                const zenith::ai::TokenCallback& = nullptr) override {
        using namespace zenith;
        // 极简演示逻辑：消息里含 "search" → 调工具；否则收尾
        ai::ChatResult r;
        r.ok = true;
        r.model = "zenith-mock";
        const std::string last = req.messages.empty() ? "" : req.messages.back().content;
        if (last.find("Tool observation") == std::string::npos &&
            last.find("weather") != std::string::npos) {
            r.content = R"({"action":"tool","tool":"get_weather","args":{"city":"Shanghai"}})";
        } else if (last.find("Tool observation") != std::string::npos) {
            r.content = R"({"action":"final","answer":"Shanghai is sunny, 26C."})";
        } else {
            r.content =
                R"({"action":"final","answer":"I am the Zenith mock backend. Ask me about weather to see the tool loop."})";
        }
        r.usage.promptTokens = 10;
        r.usage.completionTokens = 8;
        return r;
    }
    zenith::ai::EmbeddingResult embeddings(const std::string&,
                                           const std::string&) override {
        zenith::ai::EmbeddingResult e;
        e.ok = false;
        return e;
    }
    std::string backend() const override { return "mock"; }
};

// ---- 内存向量存储（生产换 db_vector 插件）----
class MemVector final : public zenith::vector::IVectorStoreService {
public:
    zenith::core::Result<void> createCollection(
        const std::string& n, const zenith::vector::CollectionConfig& cfg) override {
        zenith::vector::HnswConfig h;
        h.dim = cfg.dim;
        h.metric = cfg.metric;
        cols_[n] = std::make_unique<zenith::vector::HnswIndex>(std::move(h));
        return zenith::core::Result<void>::Ok();
    }
    zenith::core::Result<void> deleteCollection(const std::string& n) override {
        cols_.erase(n);
        return zenith::core::Result<void>::Ok();
    }
    std::vector<zenith::vector::CollectionInfo> collections() override { return {}; }
    zenith::core::Result<zenith::vector::CollectionInfo> collectionInfo(
        const std::string& n) override {
        if (cols_.count(n) == 0) {
            return zenith::core::Result<zenith::vector::CollectionInfo>::Err(
                zenith::core::Error(1, "nf"));
        }
        zenith::vector::CollectionInfo i;
        i.name = n;
        i.dim = 8;
        i.points = cols_[n]->size();
        return zenith::core::Result<zenith::vector::CollectionInfo>::Ok(i);
    }
    zenith::core::Result<void> upsert(
        const std::string& n,
        const std::vector<zenith::vector::VectorPoint>& pts) override {
        for (const auto& p : pts) cols_[n]->upsert(p.id, p.vec.data());
        return zenith::core::Result<void>::Ok();
    }
    zenith::core::Result<void> remove(const std::string& n,
                                      const std::vector<uint64_t>& ids) override {
        for (uint64_t id : ids) cols_[n]->remove(id);
        return zenith::core::Result<void>::Ok();
    }
    zenith::core::Result<zenith::vector::VectorPoint> get(const std::string&,
                                                          uint64_t) override {
        return zenith::core::Result<zenith::vector::VectorPoint>::Err(
            zenith::core::Error(1, "nf"));
    }
    zenith::core::Result<std::vector<zenith::vector::ScoredPoint>> search(
        const std::string& n, const std::vector<float>& q, int k,
        float) override {
        auto hits = cols_[n]->search(q.data(), k);
        std::vector<zenith::vector::ScoredPoint> out;
        for (const auto& h : hits) {
            zenith::vector::ScoredPoint sp;
            sp.id = h.id;
            sp.score = h.score;
            out.push_back(sp);
        }
        return zenith::core::Result<std::vector<zenith::vector::ScoredPoint>>::Ok(out);
    }
    size_t totalPoints() override { return 0; }

private:
    std::map<std::string, std::unique_ptr<zenith::vector::HnswIndex>> cols_;
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

    // ---- 1. 推理（mock 演示；生产=插件查询 IInferenceService）----
    auto inference = std::make_shared<MockInference>();

    // ---- 2. 工具协议：MCP server（把本地工具暴露给任意 Agent 客户端）----
    auto mcpServer = std::make_shared<mcp::McpServer>("zenith-agent-demo", "0.1.0");
    mcpServer->bridgeInference(static_cast<void*>(inference.get()));
    mcp::mountMcpApi(server.router(), mcpServer);

    // ---- 3. 记忆检索：向量 API（Qdrant 兼容；生产=db_vector 插件）----
    auto vecStore = std::make_shared<MemVector>();
    vector::mountVectorApi(server.router(), vecStore);

    // ---- 4. 编排：Agent（ReAct 工具循环 + 会话记忆）----
    agent::AgentConfig agCfg;
    agCfg.maxSteps = 6;
    agent::Agent ag(inference, agCfg);
    ag.registerTool("get_weather", "query weather of a city",
                     [](const http::json::JsonValue& args) {
                         return "sunny, 26C (mock weather service)";
                     });

    concurrency::ThreadPool workers(2); // 推理/工具在独立线程跑，不占 IO loop
    server.router().post("/agent/run", [&ag, &workers](http::Context& c) {
        auto body = c.bodyJson();
        const std::string msg = body["message"].asString();
        const std::string session = body["session"].asString();
        if (msg.empty()) {
            c.status(400).text(400, "400 message required");
            return;
        }
        // 快照捕获：ctx 生命周期短于异步任务——结果回写经自持结构
        auto resPtr = std::make_shared<agent::AgentResult>();
        auto done = std::make_shared<std::atomic<bool>>(false);
        workers.post([&ag, msg, session, resPtr, done] {
            *resPtr = ag.run(msg, session);
            done->store(true);
        });
        // 轮询收敛（演示级；生产可用回调+Connection 或 future 化）
        for (int i = 0; i < 2000 && !done->load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (!done->load()) {
            c.status(504).text(504, "504 agent timeout");
            return;
        }
        http::json::JsonValue v = http::json::JsonValue::object();
        v["ok"] = resPtr->ok;
        v["answer"] = resPtr->answer;
        v["tool_calls"] = static_cast<int64_t>(resPtr->toolCalls);
        v["steps"] = static_cast<int64_t>(resPtr->steps.size());
        v["error"] = resPtr->error;
        c.json(200, v);
    });

    server.router().get("/agent/tools", [&ag](http::Context& c) {
        http::json::JsonValue arr = http::json::JsonValue::array();
        for (const auto& [name, desc] : ag.toolList()) {
            http::json::JsonValue o = http::json::JsonValue::object();
            o["name"] = name;
            o["description"] = desc;
            arr.push_back(std::move(o));
        }
        c.json(200, arr);
    });

    if (!server.listen("0.0.0.0", 8092)) return 1;
    ZLOG_INFO("Zenith AI Agent demo on http://127.0.0.1:8092");
    ZLOG_INFO("  POST /agent/run   {\"message\":\"weather in shanghai?\"}");
    ZLOG_INFO("  POST /mcp         MCP JSON-RPC (tools/list)");
    ZLOG_INFO("  PUT  /collections Qdrant-compatible vector API");

    net::installSignalHandler([&] {
        server.beginGracefulShutdown([&] { loop.stop(); });
    });
    loop.loop();
    io.stop();
    workers.shutdown();
    facade.shutdown();
    return 0;
}
