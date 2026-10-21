// ============================================================================
//  M8.1 ai_inference 全链路测试
//  真插件（dlopen）→ ServiceRegistry → mountOpenAiApi → 真实 HttpServer 回环
//  覆盖：/v1/models、chat 非流式（就地 / 线程池）、chat 流式 SSE、
//        /v1/completions、/v1/embeddings、参数校验、Prometheus 打点
// ============================================================================
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ztest.hpp"

#include "zenith/ai/openai_api.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/json/json.hpp"
#include "zenith/metrics/metrics.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

using Json = http::json::JsonValue;

/// 起一套「插件 + OpenAI 路由 + HttpServer」的完整夹具
struct GatewayFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    core::KeyValueConfig cfg;
    concurrency::ThreadPool pool{2};
    plugin::ServiceRegistry registry;
    std::unique_ptr<plugin::PluginManager> mgr;
    ai::IInferenceService* service = nullptr;
    std::thread runner;

    explicit GatewayFixture(bool usePool,
                            const std::vector<std::pair<std::string, std::string>>& overrides = {}) {
        io.start(); // 连接分发线程组（漏掉它 → 请求只会在 backlog 里躺着超时）

        cfg.set("ai.inference.backend", "mock");
        cfg.set("ai.inference.default_model", "zenith-test-model");
        cfg.set("ai.inference.embedding_dim", "64");
        for (const auto& [k, v] : overrides) cfg.set(k, v);

        mgr = std::make_unique<plugin::PluginManager>(registry, &pool, &cfg);
        auto loaded = mgr->loadDirectory(ZENITH_TEST_PLUGIN_DIR);
        if (loaded.isErr()) {
            std::printf("[debug] loadDirectory failed: %s\n", loaded.error().message.c_str());
        }

        void* raw = registry.queryRaw(ai::IInferenceService::service_name(),
                                      ai::IInferenceService::service_version());
        service = static_cast<ai::IInferenceService*>(raw);
        ZEXPECT_TRUE(service != nullptr);
        if (service == nullptr) return;

        ai::OpenAiApiOptions opt;
        opt.pool = usePool ? &pool : nullptr;
        ai::mountOpenAiApi(server.router(), *service, opt);

        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }

    ~GatewayFixture() {
        loop.stop();
        if (runner.joinable()) runner.join();
        io.stop();
        if (mgr) mgr->unloadAll();
    }

    [[nodiscard]] std::string base() const {
        return "http://127.0.0.1:" + std::to_string(server.port());
    }

    core::Result<http::HttpResponse> post(const std::string& path, const std::string& body) {
        http::ClientRequest rq;
        rq.url = base() + path;
        rq.method = http::HttpMethod::Post;
        rq.body = body;
        rq.headers.set("Content-Type", "application/json");
        auto res = http::HttpClient::fetchBlocking(rq);
        if (res.isErr()) {
            std::printf("[debug] POST %s failed: %s\n", path.c_str(),
                        res.error().message.c_str());
        }
        return res;
    }

    core::Result<http::HttpResponse> get(const std::string& path) {
        http::ClientRequest rq;
        rq.url = base() + path;
        rq.method = http::HttpMethod::Get;
        auto res = http::HttpClient::fetchBlocking(rq);
        if (res.isErr()) {
            std::printf("[debug] GET %s failed: %s\n", path.c_str(),
                        res.error().message.c_str());
        }
        return res;
    }
};

/// 取出 SSE 里全部 data: 载荷（不含 [DONE]）
std::vector<std::string> ssePayloads(const std::string& body) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos < body.size()) {
        size_t eol = body.find('\n', pos);
        if (eol == std::string::npos) eol = body.size();
        std::string line = body.substr(pos, eol - pos);
        pos = eol + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("data: ", 0) == 0) out.push_back(line.substr(6));
    }
    return out;
}

} // namespace

ZTEST(OpenAi, ModelsList) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    auto res = f.get("/v1/models");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);

    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const Json& root = parsed.value();
    ZEXPECT_EQ(root["object"].asString(), std::string("list"));
    const auto* data = root["data"].asArray();
    ZEXPECT_TRUE(data != nullptr);
    if (data == nullptr || data->empty()) return;
    ZEXPECT_EQ((*data)[0]["id"].asString(), std::string("zenith-test-model"));
    ZEXPECT_EQ((*data)[0]["object"].asString(), std::string("model"));
    ZEXPECT_EQ((*data)[0]["owned_by"].asString(), std::string("zenith"));
}

ZTEST(OpenAi, ChatNonStreamInline) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    const std::string body =
        R"({"model":"zenith-test-model","messages":[{"role":"user","content":"hello"}],)"
        R"("max_tokens":64})";
    auto res = f.post("/v1/chat/completions", body);
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);

    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const Json& root = parsed.value();
    ZEXPECT_EQ(root["object"].asString(), std::string("chat.completion"));
    ZEXPECT_TRUE(root["id"].asString().rfind("chatcmpl-", 0) == 0);
    ZEXPECT_EQ(root["model"].asString(), std::string("zenith-test-model"));

    const auto* choices = root["choices"].asArray();
    ZEXPECT_TRUE(choices != nullptr && choices->size() == 1);
    if (choices == nullptr || choices->empty()) return;
    ZEXPECT_EQ((*choices)[0]["message"]["role"].asString(), std::string("assistant"));
    ZEXPECT_EQ((*choices)[0]["message"]["content"].asString(), std::string("mock: hello"));
    ZEXPECT_EQ((*choices)[0]["finish_reason"].asString(), std::string("stop"));

    ZEXPECT_TRUE(root["usage"]["prompt_tokens"].asInt() > 0);
    ZEXPECT_TRUE(root["usage"]["completion_tokens"].asInt() > 0);
    ZEXPECT_TRUE(root["usage"]["total_tokens"].asInt() >=
                 root["usage"]["completion_tokens"].asInt());
}

ZTEST(OpenAi, ChatStreamSse) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    const std::string body =
        R"({"messages":[{"role":"user","content":"hello"}],"max_tokens":64,"stream":true})";
    auto res = f.post("/v1/chat/completions", body);
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);
    ZEXPECT_TRUE(res.value().headers.get("Content-Type").find("text/event-stream") == 0);

    const std::vector<std::string> payloads = ssePayloads(res.value().body);
    ZEXPECT_TRUE(payloads.size() >= 3); // 至少：若干 delta + finish + usage + [DONE]
    if (payloads.empty()) return;
    ZEXPECT_EQ(payloads.back(), std::string("[DONE]"));

    std::string assembled;
    std::string finishReason;
    for (const std::string& p : payloads) {
        if (p == "[DONE]") continue;
        auto chunk = Json::parse(p);
        ZEXPECT_TRUE(chunk.isOk());
        if (chunk.isErr()) continue;
        const auto* choices = chunk.value()["choices"].asArray();
        if (choices == nullptr || choices->empty()) continue; // usage 片
        const Json& delta = (*choices)[0]["delta"]["content"];
        if (delta.isString()) assembled += delta.asString();
        const Json& fr = (*choices)[0]["finish_reason"];
        if (fr.isString()) finishReason = fr.asString();
    }
    ZEXPECT_EQ(assembled, std::string("mock: hello"));
    ZEXPECT_EQ(finishReason, std::string("stop"));
}

ZTEST(OpenAi, ChatNonStreamWithThreadPool) {
    GatewayFixture f(true); // 推理出 EventLoop → 走 chunked 延迟响应
    if (f.service == nullptr) return;
    const std::string body = R"({"messages":[{"role":"user","content":"pooled"}]})";
    auto res = f.post("/v1/chat/completions", body);
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);

    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const auto* choices = parsed.value()["choices"].asArray();
    ZEXPECT_TRUE(choices != nullptr && !choices->empty());
    if (choices == nullptr || choices->empty()) return;
    ZEXPECT_EQ((*choices)[0]["message"]["content"].asString(), std::string("mock: pooled"));
}

ZTEST(OpenAi, Completions) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    auto res = f.post("/v1/completions", R"({"prompt":"hi","max_tokens":32})");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);
    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    ZEXPECT_EQ(parsed.value()["object"].asString(), std::string("text_completion"));
    const auto* choices = parsed.value()["choices"].asArray();
    ZEXPECT_TRUE(choices != nullptr && !choices->empty());
    if (choices == nullptr || choices->empty()) return;
    ZEXPECT_EQ((*choices)[0]["text"].asString(), std::string("mock: hi"));
}

ZTEST(OpenAi, EmbeddingsAreUnitVectors) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    auto res = f.post("/v1/embeddings", R"({"input":["alpha","beta"]})");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) return;
    ZEXPECT_EQ(res.value().status, 200);

    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const auto* data = parsed.value()["data"].asArray();
    ZEXPECT_TRUE(data != nullptr && data->size() == 2);
    if (data == nullptr || data->empty()) return;

    const auto* vec = (*data)[0]["embedding"].asArray();
    ZEXPECT_TRUE(vec != nullptr && vec->size() == 64);
    if (vec == nullptr) return;
    double norm = 0.0;
    for (const auto& v : *vec) norm += v.asDouble() * v.asDouble();
    ZEXPECT_TRUE(std::fabs(std::sqrt(norm) - 1.0) < 1e-3); // 单位向量

    // 确定性：同一输入两次结果一致
    auto again = f.post("/v1/embeddings", R"({"input":["alpha","beta"]})");
    ZEXPECT_TRUE(again.isOk());
    if (again.isOk()) {
        auto parsed2 = Json::parse(again.value().body);
        ZEXPECT_TRUE(parsed2.isOk());
        if (parsed2.isOk()) {
            // 同一输入必须产出同一向量（整段 data 逐一比对）
            ZEXPECT_EQ(parsed2.value()["data"].dump(), parsed.value()["data"].dump());
        }
    }
}

ZTEST(OpenAi, RejectsBadRequests) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;

    // 1) 非 JSON
    auto r1 = f.post("/v1/chat/completions", "not-json");
    ZEXPECT_TRUE(r1.isOk());
    if (r1.isOk()) {
        ZEXPECT_EQ(r1.value().status, 400);
        auto parsed = Json::parse(r1.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            ZEXPECT_EQ(parsed.value()["error"]["type"].asString(),
                       std::string("invalid_request_error"));
        }
    }
    // 2) 缺 messages
    auto r2 = f.post("/v1/chat/completions", R"({"model":"x"})");
    ZEXPECT_TRUE(r2.isOk());
    if (r2.isOk()) {
        ZEXPECT_EQ(r2.value().status, 400);
        ZEXPECT_TRUE(r2.value().body.find("messages") != std::string::npos);
    }
    // 3) 缺 input
    auto r3 = f.post("/v1/embeddings", R"({})");
    ZEXPECT_TRUE(r3.isOk());
    if (r3.isOk()) ZEXPECT_EQ(r3.value().status, 400);
}

ZTEST(OpenAi, PrometheusCountersAdvance) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;

    auto& reg = metrics::Registry::instance();
    const int64_t beforeReq = reg.counter("zenith_ai_requests_total").value();
    const int64_t beforeTok = reg.counter("zenith_ai_completion_tokens_total").value();

    auto res = f.post("/v1/chat/completions", R"({"messages":[{"role":"user","content":"md"}]})");
    ZEXPECT_TRUE(res.isOk());

    ZEXPECT_TRUE(reg.counter("zenith_ai_requests_total").value() > beforeReq);
    ZEXPECT_TRUE(reg.counter("zenith_ai_chat_requests_total").value() > 0);
    ZEXPECT_TRUE(reg.counter("zenith_ai_completion_tokens_total").value() > beforeTok);
    const std::string prom = reg.exportPrometheus();
    ZEXPECT_TRUE(prom.find("zenith_ai_requests_total") != std::string::npos);
}

ZTEST(OpenAi, ServiceBackendReported) {
    GatewayFixture f(false);
    if (f.service == nullptr) return;
    ZEXPECT_EQ(f.service->backend(), std::string("mock"));
    ZEXPECT_TRUE(!f.service->models().empty());
}

// ---------------------------------------------------------------------------
//  活体联调：对接真实 OpenAI 兼容上游
//  本机用法（先按 启动Bonsai2-27B.ps1 起 Prism llama-server，建议加
//  --reasoning-budget 0 与较小的 -c 以便快速冒烟）：
//      set ZENITH_TEST_UPSTREAM_URL=http://127.0.0.1:8080
//      set ZENITH_TEST_UPSTREAM_KEY=<--api-key 的值>
//      set ZENITH_TEST_UPSTREAM_MODEL=Ternary-Bonsai-2-27B
//  未设置环境变量时自动跳过，因此不影响 CI。
// ---------------------------------------------------------------------------
ZTEST(OpenAi, LiveUpstreamOpenAiCompat) {
    const char* url = std::getenv("ZENITH_TEST_UPSTREAM_URL");
    if (url == nullptr || *url == '\0') {
        std::printf("[ SKIP ] ZENITH_TEST_UPSTREAM_URL not set\n");
        return;
    }
    const char* keyEnv = std::getenv("ZENITH_TEST_UPSTREAM_KEY");
    const char* modelEnv = std::getenv("ZENITH_TEST_UPSTREAM_MODEL");
    const std::string model = modelEnv != nullptr ? modelEnv : "Ternary-Bonsai-2-27B";

    GatewayFixture f(false,
                     {{"ai.inference.backend", "openai"},
                      {"ai.inference.upstream_url", url},
                      {"ai.inference.upstream_api_key", keyEnv != nullptr ? keyEnv : ""},
                      {"ai.inference.upstream_timeout_ms", "600000"},
                      {"ai.inference.default_model", model}});
    if (f.service == nullptr) return;
    ZEXPECT_EQ(f.service->backend(), std::string("openai-upstream"));

    // 1) 模型列表来自上游
    auto models = f.get("/v1/models");
    ZEXPECT_TRUE(models.isOk());
    if (models.isOk()) {
        ZEXPECT_EQ(models.value().status, 200);
        auto parsed = Json::parse(models.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            const auto* data = parsed.value()["data"].asArray();
            ZEXPECT_TRUE(data != nullptr && !data->empty());
        }
    }

    // 2) 非流式对话（思考模型的 reasoning_content 应被单独透出）
    const std::string body = R"({"messages":[{"role":"user","content":"用一句话说明你是谁"}],)"
                             R"("max_tokens":512,"temperature":0.3})";
    auto res = f.post("/v1/chat/completions", body);
    ZEXPECT_TRUE(res.isOk());
    if (res.isOk()) {
        ZEXPECT_EQ(res.value().status, 200);
        auto parsed = Json::parse(res.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            const auto* choices = parsed.value()["choices"].asArray();
            ZEXPECT_TRUE(choices != nullptr && !choices->empty());
            if (choices != nullptr && !choices->empty()) {
                const std::string content = (*choices)[0]["message"]["content"].asString();
                const std::string reasoning =
                    (*choices)[0]["message"]["reasoning_content"].asString();
                std::printf("[ live ] 非流式回复(%zu 字节): %.120s\n", content.size(),
                            content.c_str());
                if (!reasoning.empty()) {
                    std::printf("[ live ] 推理过程(%zu 字节): %.80s\n", reasoning.size(),
                                reasoning.c_str());
                }
                ZEXPECT_TRUE(!content.empty());
            }
            ZEXPECT_TRUE(parsed.value()["usage"]["total_tokens"].asInt() > 0);
        }
    }

    // 3) 流式对话（SSE 逐片回放；思考片走 reasoning_content 帧）
    const std::string sbody = R"({"messages":[{"role":"user","content":"数到五"}],)"
                              R"("max_tokens":512,"stream":true})";
    auto sres = f.post("/v1/chat/completions", sbody);
    ZEXPECT_TRUE(sres.isOk());
    if (sres.isOk()) {
        ZEXPECT_EQ(sres.value().status, 200);
        const std::vector<std::string> payloads = ssePayloads(sres.value().body);
        std::string assembled;
        std::string thinking;
        for (const std::string& p : payloads) {
            if (p == "[DONE]") continue;
            auto chunk = Json::parse(p);
            if (chunk.isErr()) continue;
            const auto* choices = chunk.value()["choices"].asArray();
            if (choices == nullptr || choices->empty()) continue;
            const Json& delta = (*choices)[0]["delta"]["content"];
            if (delta.isString()) assembled += delta.asString();
            const Json& rea = (*choices)[0]["delta"]["reasoning_content"];
            if (rea.isString()) thinking += rea.asString();
        }
        std::printf("[ live ] 流式分片 %zu 个，正文字节 %zu，思考字节 %zu: %.120s\n",
                    payloads.size(), assembled.size(), thinking.size(), assembled.c_str());
        ZEXPECT_TRUE(payloads.size() > 1);
        ZEXPECT_TRUE(!assembled.empty());
        ZEXPECT_EQ(payloads.back(), std::string("[DONE]"));
    }
}

// ---------------------------------------------------------------------------
//  活体联调 B：本地引擎进程监管（NInfer / llama-server）
//  本机用法（以 infer-engine-sm120a 包为例）：
//    set ZENITH_TEST_ENGINE_EXE=C:\...\engine\ninfer-serve-120a.exe
//    set ZENITH_TEST_ENGINE_MODEL=C:\...\models\Ternary-Bonsai-2-27B-ninfer-v3-mtponly.ninfer
//  其余（args/env/端口/model-id）有默认值，见 envOr 调用处。
//  未设置 ZENITH_TEST_ENGINE_EXE 时自动跳过。
//  依包内教程 §0：只做「一条 1000 进 / 1000 出」的测速，不做穷举矩阵。
// ---------------------------------------------------------------------------
namespace {
std::string countingPrompt() {
    std::string p;
    for (int i = 1; i <= 300; ++i) {
        p += std::to_string(i);
        p += ' ';
    }
    p += "\n\nContinue the same number sequence from the next number. Output numbers only, in "
         "the same space-separated format, and keep going until the length limit.";
    return p;
}
} // namespace

ZTEST(OpenAi, LiveSupervisedEngine) {
    const char* exe = std::getenv("ZENITH_TEST_ENGINE_EXE");
    if (exe == nullptr || *exe == '\0') {
        std::printf("[ SKIP ] ZENITH_TEST_ENGINE_EXE not set\n");
        return;
    }
    const auto envOr = [](const char* k, const std::string& def) -> std::string {
        const char* v = std::getenv(k);
        return (v != nullptr && *v != '\0') ? std::string(v) : def;
    };
    const std::string modelPath = envOr("ZENITH_TEST_ENGINE_MODEL", "");
    const std::string modelId = envOr("ZENITH_TEST_ENGINE_MODEL_ID", "qwen3.8-27b");
    const std::string port = envOr("ZENITH_TEST_ENGINE_PORT", "8111");
    const std::string args =
        envOr("ZENITH_TEST_ENGINE_ARGS",
              "<MODEL> --host 127.0.0.1 --port " + port + " --model-id " + modelId +
                  " --max-context 32768 --kv-capacity 8192 --kv-dtype k8v4 --host-kv-mib 4096"
                  " --prefill-chunk 1024 --spec mtp --draft-tokens 4 --default-max-tokens 32768"
                  " --default-reasoning-effort none --max-concurrency 1 --max-shared-prefixes 0"
                  " --presence-penalty 0 --temperature 0.7 --top-p 0.9 --top-k 20");
    const std::string engineEnv = envOr(
        "ZENITH_TEST_ENGINE_ENV",
        "NINFER_KV_WINDOW=16384;NINFER_KV_RETRIEVE=8192;NINFER_KV_RING=1;"
        "NINFER_HOST_PAGEABLE=1;NINFER_KV_REUSE_HOSTBACKED=1");

    GatewayFixture f(false, {{"ai.inference.backend", "engine"},
                             {"ai.inference.engine.exe", exe},
                             {"ai.inference.engine.model", modelPath},
                             {"ai.inference.engine.args", args},
                             {"ai.inference.engine.env", engineEnv},
                             {"ai.inference.default_model", modelId},
                             {"ai.inference.upstream_timeout_ms", "900000"}});
    if (f.service == nullptr) return;

    // 1) 引擎被拉起且就绪
    const ai::EngineState st = f.service->engineState();
    std::fprintf(stderr, "[ engine ] pid=%s running=%s supervised=%s last=%s\n",
                 std::to_string(st.pid).c_str(), st.running ? "1" : "0",
                 st.supervised ? "1" : "0", st.lastLine.c_str());
    ZEXPECT_TRUE(st.supervised);
    ZEXPECT_TRUE(st.running);

    // 2) 网关 status 端点反映引擎运行态
    auto status = f.get("/zenith/ai/status");
    ZEXPECT_TRUE(status.isOk());
    if (status.isOk()) {
        auto parsed = Json::parse(status.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            ZEXPECT_TRUE(parsed.value()["engine"]["running"].asBool(false));
            std::printf("[ status ] backend=%s models=%s\n",
                        parsed.value()["backend"].asString().c_str(),
                        parsed.value()["models"].dump().c_str());
        }
    }

    // 3) 教程 §0.2 的唯一一条测速：1000 进 / 1000 出
    Json body = Json::JsonValue::object();
    body["model"] = modelId;
    body["max_tokens"] = 1000;
    body["temperature"] = 0.0;
    Json msgs = Json::JsonValue::array();
    Json m = Json::JsonValue::object();
    m["role"] = "user";
    m["content"] = countingPrompt();
    msgs.push_back(std::move(m));
    body["messages"] = std::move(msgs);

    const auto t0 = std::chrono::steady_clock::now();
    auto res = f.post("/v1/chat/completions", body.dump());
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - t0)
                               .count();
    ZEXPECT_TRUE(res.isOk());
    if (res.isOk()) {
        ZEXPECT_EQ(res.value().status, 200);
        auto parsed = Json::parse(res.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            const auto* choices = parsed.value()["choices"].asArray();
            ZEXPECT_TRUE(choices != nullptr && !choices->empty());
            if (choices != nullptr && !choices->empty()) {
                const std::string content = (*choices)[0]["message"]["content"].asString();
                std::printf("[ engine ] 测速：%lld ms 总耗时，正文 %zu 字节：%.80s\n",
                            static_cast<long long>(elapsedMs), content.size(), content.c_str());
                ZEXPECT_TRUE(content.find("301") != std::string::npos);
            }
            if (const auto* timings = parsed.value().find("timings"); timings != nullptr) {
                std::printf("[ engine ] prefill=%.1f tok/s decode=%.1f tok/s draft=%lld/%lld\n",
                            (*timings)["prompt_per_second"].asDouble(),
                            (*timings)["predicted_per_second"].asDouble(),
                            static_cast<long long>((*timings)["draft_n"].asInt()),
                            static_cast<long long>((*timings)["draft_n_accepted"].asInt()));
            }
        }
    }

    // 4) 引擎控制台解析出的运行态（TTFT/速率/投机接受）
    const ai::EngineState after = f.service->engineState();
    std::printf(
        "[ engine ] 控制台：reqs=%lld ttft=%.0fms prefill=%.0f decode=%.1f acc=%lld/%lld\n",
        static_cast<long long>(after.requests), after.lastTtftMs, after.lastPrefillTps,
        after.lastDecodeTps, static_cast<long long>(after.draftAccepted),
        static_cast<long long>(after.draftTotal));
    ZEXPECT_TRUE(after.requests >= 1);

    // 5) 流式通道可用（功能性验证，非性能矩阵）
    const std::string sbody = R"({"messages":[{"role":"user","content":"数到十"}],)"
                              R"("max_tokens":64,"stream":true})";
    auto sres = f.post("/v1/chat/completions", sbody);
    ZEXPECT_TRUE(sres.isOk());
    if (sres.isOk()) {
        ZEXPECT_EQ(sres.value().status, 200);
        const std::vector<std::string> payloads = ssePayloads(sres.value().body);
        std::printf("[ engine ] 流式分片 %zu 个，末片=%s\n", payloads.size(),
                    payloads.empty() ? "(none)" : payloads.back().c_str());
        ZEXPECT_TRUE(payloads.size() > 1);
    }
}

ZTEST_MAIN
