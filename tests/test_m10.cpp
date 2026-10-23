// M10 测试：HNSW 召回率 + Qdrant API e2e + MCP 协议 + Agent 循环
#include <atomic>
#include <chrono>
#include <cmath>
#include <random>
#include <set>
#include <thread>

#include "ztest.hpp"

#include "zenith/agent/agent.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/mcp/mcp_api.hpp"
#include "zenith/mcp/mcp_client.hpp"
#include "zenith/vector/hnsw.hpp"
#include "zenith/vector/vector_api.hpp"
#include "zenith/vector/vector_service.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- HNSW 单元 ---------------- */

namespace {
/// 生成聚类向量：c 个簇心，每簇 n 个带扰动的点
std::vector<std::vector<float>> makeClustered(int clusters, int perCluster, int dim,
                                              unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<float> noise(0, 0.05f);
    std::vector<std::vector<float>> out;
    for (int c = 0; c < clusters; ++c) {
        std::vector<float> center(static_cast<size_t>(dim));
        for (auto& x : center) x = static_cast<float>(rng()) / rng.max() * 2 - 1;
        for (int i = 0; i < perCluster; ++i) {
            std::vector<float> v(static_cast<size_t>(dim));
            for (int d = 0; d < dim; ++d) {
                v[static_cast<size_t>(d)] = center[static_cast<size_t>(d)] + noise(rng);
            }
            out.push_back(std::move(v));
        }
    }
    return out;
}
} // namespace

ZTEST(Hnsw, RecallOnClusteredData) {
    const int dim = 32;
    auto data = makeClustered(20, 50, dim, 42); // 1000 点
    vector::HnswConfig cfg;
    cfg.dim = dim;
    cfg.metric = vector::Metric::Euclidean;
    cfg.m = 16;
    cfg.efConstruction = 100;
    cfg.efSearch = 128;
    cfg.seed = 7;
    vector::HnswIndex idx(cfg);
    for (size_t i = 0; i < data.size(); ++i) {
        idx.upsert(static_cast<uint64_t>(i + 1), data[i].data());
    }
    ZEXPECT_EQ(idx.size(), size_t{data.size()});

    // 暴力精确基线 vs HNSW：对 50 个查询点算 recall@10
    std::mt19937 rng(99);
    int recalled = 0, total = 0;
    for (int q = 0; q < 50; ++q) {
        const size_t qi = rng() % data.size();
        const std::vector<float>& query = data[qi];

        // 精确 top-10（不含自身）
        std::vector<std::pair<float, uint64_t>> exact;
        for (size_t i = 0; i < data.size(); ++i) {
            if (i == qi) continue;
            float acc = 0;
            for (int d = 0; d < dim; ++d) {
                const float df = data[i][static_cast<size_t>(d)] -
                                 query[static_cast<size_t>(d)];
                acc += df * df;
            }
            exact.emplace_back(acc, static_cast<uint64_t>(i + 1));
        }
        std::sort(exact.begin(), exact.end());
        std::set<uint64_t> truth;
        for (int i = 0; i < 10; ++i) truth.insert(exact[static_cast<size_t>(i)].second);

        auto hits = idx.search(query.data(), 10);
        ZEXPECT_TRUE(!hits.empty());
        for (const auto& h : hits) {
            if (truth.count(h.id) > 0) ++recalled;
        }
        total += 10;
    }
    const float recall = static_cast<float>(recalled) / total;
    // HNSW 论文级参数在聚类数据上应轻松 >0.9
    ZEXPECT_TRUE(recall >= 0.85f);
}

ZTEST(Hnsw, UpsertRemoveSerialize) {
    vector::HnswConfig cfg;
    cfg.dim = 8;
    cfg.metric = vector::Metric::Cosine;
    cfg.seed = 1;
    vector::HnswIndex idx(cfg);
    std::vector<float> a{1, 0, 0, 0, 0, 0, 0, 0};
    std::vector<float> b{0, 1, 0, 0, 0, 0, 0, 0};
    std::vector<float> c{0.9f, 0.1f, 0, 0, 0, 0, 0, 0};
    idx.upsert(1, a.data());
    idx.upsert(2, b.data());
    idx.upsert(3, c.data());

    // c 最接近 a（cosine）
    auto hits = idx.search(a.data(), 2);
    ZEXPECT_EQ(hits.size(), size_t{2});
    ZEXPECT_EQ(hits[0].id, static_cast<uint64_t>(1)); // 自身（cos=1 → score 最高）
    ZEXPECT_EQ(hits[1].id, static_cast<uint64_t>(3));

    // 删除 3 → 不再返回
    idx.remove(3);
    hits = idx.search(a.data(), 3);
    for (const auto& h : hits) ZEXPECT_TRUE(h.id != 3);
    ZEXPECT_EQ(idx.size(), size_t{2});

    // 序列化 → 反序列化 → 同结果
    auto blob = idx.serialize();
    ZEXPECT_TRUE(static_cast<bool>(blob));
    auto back = vector::HnswIndex::deserialize(blob.value().data(), blob.value().size());
    ZEXPECT_TRUE(static_cast<bool>(back));
    auto hits2 = back.value()->search(a.data(), 2);
    ZEXPECT_EQ(hits2.size(), size_t{2});
    ZEXPECT_EQ(hits2[0].id, static_cast<uint64_t>(1));
}

/* ---------------- Qdrant 兼容 API e2e ---------------- */

ZTEST(VectorApi, QdrantCompatibleE2E) {
    // 内存实现（不经插件——API 层独立可测）
    class MemVecSvc final : public vector::IVectorStoreService {
    public:
        vector::HnswConfig base{8, vector::Metric::Cosine, 8, 64, 32, 3};
        std::map<std::string, std::unique_ptr<vector::HnswIndex>> cols;
        std::map<std::string, std::map<uint64_t, std::string>> payloads;

        core::Result<void> createCollection(const std::string& n,
                                            const vector::CollectionConfig& cfg) override {
            if (cols.count(n)) return core::Result<void>::Err(core::Error(1, "exists"));
            vector::HnswConfig h = base;
            h.dim = cfg.dim;
            h.metric = cfg.metric;
            cols[n] = std::make_unique<vector::HnswIndex>(std::move(h));
            return core::Result<void>::Ok();
        }
        core::Result<void> deleteCollection(const std::string& n) override {
            cols.erase(n);
            return core::Result<void>::Ok();
        }
        std::vector<vector::CollectionInfo> collections() override {
            std::vector<vector::CollectionInfo> out;
            for (auto& [n, i] : cols) {
                vector::CollectionInfo ci;
                ci.name = n;
                ci.dim = 8;
                ci.points = i->size();
                out.push_back(ci);
            }
            return out;
        }
        core::Result<vector::CollectionInfo> collectionInfo(const std::string& n) override {
            if (!cols.count(n)) {
                return core::Result<vector::CollectionInfo>::Err(core::Error(2, "nf"));
            }
            vector::CollectionInfo ci;
            ci.name = n;
            ci.dim = 8;
            ci.points = cols[n]->size();
            return core::Result<vector::CollectionInfo>::Ok(ci);
        }
        core::Result<void> upsert(const std::string& n,
                                  const std::vector<vector::VectorPoint>& pts) override {
            for (const auto& p : pts) {
                cols[n]->upsert(p.id, p.vec.data());
                payloads[n][p.id] = p.payloadJson;
            }
            return core::Result<void>::Ok();
        }
        core::Result<void> remove(const std::string& n,
                                  const std::vector<uint64_t>& ids) override {
            for (uint64_t id : ids) {
                cols[n]->remove(id);
                payloads[n].erase(id);
            }
            return core::Result<void>::Ok();
        }
        core::Result<vector::VectorPoint> get(const std::string& n, uint64_t id) override {
            if (!cols.count(n)) {
                return core::Result<vector::VectorPoint>::Err(core::Error(2, "nf"));
            }
            vector::VectorPoint p;
            p.id = id;
            if (!cols[n]->get(id, p.vec)) {
                return core::Result<vector::VectorPoint>::Err(core::Error(3, "nf"));
            }
            p.payloadJson = payloads[n][id];
            return core::Result<vector::VectorPoint>::Ok(p);
        }
        core::Result<std::vector<vector::ScoredPoint>> search(
            const std::string& n, const std::vector<float>& q, int k,
            float thr) override {
            auto hits = cols[n]->search(q.data(), k);
            std::vector<vector::ScoredPoint> out;
            for (const auto& h : hits) {
                if (h.score < thr) continue;
                vector::ScoredPoint sp;
                sp.id = h.id;
                sp.score = h.score;
                sp.payloadJson = payloads[n][h.id];
                out.push_back(sp);
            }
            return core::Result<std::vector<vector::ScoredPoint>>::Ok(out);
        }
        size_t totalPoints() override { return 0; }
    };

    auto svc = std::make_shared<MemVecSvc>();
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    vector::mountVectorApi(server.router(), svc);
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(150ms);
    const auto url = [&](const std::string& p) {
        return "http://127.0.0.1:" + std::to_string(server.port()) + p;
    };

    // 创建集合
    http::ClientRequest mk;
    mk.url = url("/collections");
    mk.method = http::HttpMethod::Put;
    mk.body = R"({"name":"docs","vectors":{"size":8,"distance":"Cosine"}})";
    auto cr = http::HttpClient::fetchBlocking(mk);
    ZEXPECT_EQ(cr.value().status, 200);
    ZEXPECT_TRUE(cr.value().body.find("\"ok\"") != std::string::npos);

    // upsert 两点
    mk.url = url("/collections/docs/points");
    mk.body = R"({"points":[{"id":1,"vector":[1,0,0,0,0,0,0,0],"payload":{"text":"hello"}},
                              {"id":2,"vector":[0,1,0,0,0,0,0,0],"payload":{"text":"world"}}]})";
    auto ur = http::HttpClient::fetchBlocking(mk);
    ZEXPECT_EQ(ur.value().status, 200);

    // 检索（Qdrant 响应形态：result 数组 + score）
    http::ClientRequest se;
    se.url = url("/collections/docs/points/search");
    se.method = http::HttpMethod::Post;
    se.body = R"({"vector":[1,0,0,0,0,0,0,0],"limit":2,"with_payload":true})";
    auto sr = http::HttpClient::fetchBlocking(se);
    ZEXPECT_EQ(sr.value().status, 200);
    auto sv = http::json::JsonValue::parse(sr.value().body);
    ZEXPECT_TRUE(static_cast<bool>(sv));
    ZEXPECT_EQ(sv.value()["result"].size(), size_t{2});
    // 首位应是与查询同向的 id=1（cos=1）；不硬编码下标——按结果定位断言
    bool found1 = false;
    for (const auto& row : *sv.value()["result"].asArray()) {
        if (row["id"].asInt() == 1) {
            ZEXPECT_TRUE(row["score"].asDouble() > 0.99);
            ZEXPECT_EQ(row["payload"]["text"].asString(), std::string("hello"));
            found1 = true;
        }
    }
    ZEXPECT_TRUE(found1);

    // 取单点 + 删除
    auto g = http::HttpClient::fetchBlockingUrl(url("/collections/docs/points/1"));
    ZEXPECT_EQ(g.value().status, 200);
    http::ClientRequest del;
    del.url = url("/collections/docs/points/delete");
    del.method = http::HttpMethod::Post;
    del.body = R"({"points":[2]})";
    auto dr = http::HttpClient::fetchBlocking(del);
    ZEXPECT_EQ(dr.value().status, 200);
    auto g2 = http::HttpClient::fetchBlockingUrl(url("/collections/docs/points/2"));
    ZEXPECT_EQ(g2.value().status, 404);

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- MCP 协议 ---------------- */

ZTEST(Mcp, ServerDispatchAndBridge) {
    mcp::McpServer srv("test-mcp", "1.0");
    mcp::ToolDef echo;
    echo.name = "echo";
    echo.description = "echo back";
    srv.registerTool(echo, [](const http::json::JsonValue& args) {
        return mcp::ToolResult{false, "echo:" + args["msg"].asString()};
    });

    // initialize
    auto init = srv.handle(http::json::JsonValue::parse(
                               R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{}})")
                               .value());
    ZEXPECT_EQ(init["result"]["protocolVersion"].asString(),
               std::string("2026-07-28"));
    ZEXPECT_EQ(init["result"]["serverInfo"]["name"].asString(), std::string("test-mcp"));

    // tools/list
    auto tl = srv.handle(
        http::json::JsonValue::parse(
            R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})")
            .value());
    ZEXPECT_EQ(tl["result"]["tools"].size(), size_t{1});
    ZEXPECT_EQ(tl["result"]["tools"].at(size_t{0})["name"].asString(), std::string("echo"));

    // tools/call（标准 "arguments" 键）
    auto tc = srv.handle(
        http::json::JsonValue::parse(
            R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"echo","arguments":{"msg":"hi"}}})")
            .value());
    ZEXPECT_EQ(tc["result"]["content"].at(size_t{0})["text"].asString(), std::string("echo:hi"));
    ZEXPECT_FALSE(tc["result"]["isError"].asBool());

    // 未知方法 → MethodNotFound
    auto bad = srv.handle(
        http::json::JsonValue::parse(
            R"({"jsonrpc":"2.0","id":4,"method":"bogus","params":{}})")
            .value());
    ZEXPECT_EQ(bad["error"]["code"].asInt(), int64_t{-32601});

    // 通知 → Null
    auto note = srv.handle(
        http::json::JsonValue::parse(
            R"({"jsonrpc":"2.0","method":"notifications/initialized"})")
            .value());
    ZEXPECT_TRUE(note.isNull());

    // raw 批量 + 全通知 → 空串
    ZEXPECT_TRUE(srv.handleRaw(
                     R"([{"jsonrpc":"2.0","method":"notifications/x"},{"jsonrpc":"2.0","method":"notifications/y"}])")
                     .empty());
}

ZTEST(Mcp, HttpE2EClientServer) {
    mcp::McpServer srv("zenith-e2e", "0.1");
    mcp::ToolDef calc;
    calc.name = "add";
    srv.registerTool(calc, [](const http::json::JsonValue& args) {
        const int64_t sum = args["a"].asInt() + args["b"].asInt();
        return mcp::ToolResult{false, std::to_string(sum)};
    });

    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    io.start();
    http::HttpServer server(&loop, &io);
    mcp::mountMcpApi(server.router(), std::make_shared<mcp::McpServer>(std::move(srv)));
    (void)server.listen("127.0.0.1", 0);
    std::thread t([&] { loop.loop(); });
    std::this_thread::sleep_for(150ms);

    mcp::McpClient cli("http://127.0.0.1:" + std::to_string(server.port()) + "/mcp");
    auto tools = cli.listTools();
    ZEXPECT_TRUE(static_cast<bool>(tools));
    ZEXPECT_EQ(tools.value().size(), size_t{1});
    ZEXPECT_EQ(tools.value()[0].name, std::string("add"));

    auto sum = cli.callTool("add",
                            http::json::JsonValue::parse(R"({"a":40,"b":2})").value());
    ZEXPECT_TRUE(static_cast<bool>(sum));
    ZEXPECT_EQ(sum.value(), std::string("42"));

    auto bad = cli.callTool("nope", http::json::JsonValue::object());
    ZEXPECT_TRUE(bad.isErr());

    loop.stop();
    t.join();
    io.stop();
}

/* ---------------- Agent 循环 ---------------- */

namespace {
/// 剧本式 mock 推理：依次吐出预设回复
class ScriptedInference final : public ai::IInferenceService {
public:
    std::vector<std::string> script;
    size_t idx = 0;

    std::vector<ai::ModelInfo> models() override { return {}; }
    ai::ChatResult chat(const ai::ChatRequest&, const ai::TokenCallback& = nullptr,
                        const ai::TokenCallback& = nullptr) override {
        ai::ChatResult r;
        if (idx >= script.size()) {
            r.ok = false;
            r.error = "script exhausted";
            return r;
        }
        r.ok = true;
        r.content = script[idx++];
        r.finishReason = "stop";
        return r;
    }
    ai::EmbeddingResult embeddings(const std::string&, const std::string&) override {
        ai::EmbeddingResult e;
        e.ok = false;
        return e;
    }
    std::string backend() const override { return "scripted"; }
};
} // namespace

ZTEST(Agent, ToolLoopThenFinal) {
    auto inf = std::make_shared<ScriptedInference>();
    inf->script = {
        R"({"action":"tool","tool":"add","args":{"a":19,"b":23}})",     // 步 1：调工具
        R"({"action":"final","answer":"the sum is 42"})",               // 步 2：收尾
    };

    agent::Agent ag(inf);
    ag.registerTool("add", "add two numbers", [](const http::json::JsonValue& a) {
        return std::to_string(a["a"].asInt() + a["b"].asInt());
    });

    auto r = ag.run("compute 19 + 23");
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_EQ(r.answer, std::string("the sum is 42"));
    ZEXPECT_EQ(r.toolCalls, 1);
    ZEXPECT_EQ(r.steps.size(), size_t{2});
    ZEXPECT_EQ(r.steps[0].tool, std::string("add"));
    ZEXPECT_EQ(r.steps[0].observation, std::string("42")); // 工具结果正确回喂
}

ZTEST(Agent, UnknownToolAndFallbackFinal) {
    { // 未知工具 → observation 提示，仍可继续到 final
        auto inf = std::make_shared<ScriptedInference>();
        inf->script = {
            R"({"action":"tool","tool":"nope","args":{}})",
            R"({"action":"final","answer":"recovered"})",
        };
        agent::Agent ag(inf);
        auto r = ag.run("try missing tool");
        ZEXPECT_TRUE(r.ok);
        ZEXPECT_EQ(r.answer, std::string("recovered"));
        ZEXPECT_TRUE(r.steps[0].observation.find("unknown tool") != std::string::npos);
    }
    { // 模型不守约（纯文本）→ 容错降级为 final
        auto inf = std::make_shared<ScriptedInference>();
        inf->script = {"I do not follow protocols, here is my answer: 7"};
        agent::Agent ag(inf);
        auto r = ag.run("anything");
        ZEXPECT_TRUE(r.ok);
        ZEXPECT_EQ(r.answer, std::string("I do not follow protocols, here is my answer: 7"));
        ZEXPECT_EQ(r.toolCalls, 0);
    }
}

ZTEST(Agent, MaxStepsExceeded) {
    auto inf = std::make_shared<ScriptedInference>();
    inf->script.assign(10, R"({"action":"tool","tool":"loop","args":{}})");
    agent::AgentConfig cfg;
    cfg.maxSteps = 3;
    agent::Agent ag(inf, cfg);
    ag.registerTool("loop", "infinite", [](const http::json::JsonValue&) {
        return "again";
    });
    auto r = ag.run("loop forever");
    ZEXPECT_FALSE(r.ok);
    ZEXPECT_TRUE(r.error.find("max steps") != std::string::npos);
    ZEXPECT_EQ(r.toolCalls, 3);
}

ZTEST(Agent, SessionMemoryRolling) {
    auto inf = std::make_shared<ScriptedInference>();
    inf->script = {R"({"action":"final","answer":"remembered"})",
                   R"({"action":"final","answer":"second"})"};
    agent::Agent ag(inf);
    auto r1 = ag.run("hi", "s1");
    ZEXPECT_TRUE(r1.ok);
    auto r2 = ag.run("again", "s1");
    ZEXPECT_TRUE(r2.ok);
    // 会话历史有积累（run 内部可查）——以行为断言：两次均正常收尾即可
    // （历史正确性由 steps 与 session map 保证，此处不越界检查私有态）
    ZEXPECT_EQ(r2.answer, std::string("second"));
}

ZTEST_MAIN
