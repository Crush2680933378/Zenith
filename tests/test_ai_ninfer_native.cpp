// ============================================================================
//  ai_ninfer_native 活体测试：**进程内** NInfer（自研 C ABI shim，MSVC 侧插件）
//  与 test_ai_ninfer（子进程 + HTTP）互补：这条链路没有 loopback HTTP、没有子进程。
//  环境变量：ZENITH_TEST_NINFER_NATIVE_MODEL（未设时试默认 .ninfer 路径）
// ============================================================================
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "ztest.hpp"

#include "zenith/ai/ninfer_c_api_client.hpp"
#include "zenith/ai/openai_api.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/json/json.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;
using namespace std::chrono_literals;
using Json = http::json::JsonValue;

namespace {

/// 原生插件没编（CI 不跑 MSVC 子工程）→ 整例跳过
bool pluginPresent() {
    std::error_code ec;
    const std::string name = std::string("zenith_ai_ninfer_native") + plugin::pluginFileExtension();
    return std::filesystem::exists(std::filesystem::path(ZENITH_TEST_PLUGIN_DIR) / name, ec);
}

std::string resolveModel() {
    const char* env = std::getenv("ZENITH_TEST_NINFER_NATIVE_MODEL");
    if (env != nullptr && *env != '\0') return env;
    const char* candidate =
        "C:\\Users\\26809\\Desktop\\infer-engine-sm120a-20261002\\models\\"
        "Ternary-Bonsai-2-27B-ninfer-v3.ninfer";
    FILE* f = std::fopen(candidate, "rb");
    if (f == nullptr) return std::string();
    std::fclose(f);
    return candidate;
}

struct NativeFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    core::KeyValueConfig cfg;
    concurrency::ThreadPool pool{2};
    plugin::ServiceRegistry registry;
    std::unique_ptr<plugin::PluginManager> mgr;
    std::unique_ptr<ai::IInferenceService> service;
    std::thread runner;

    explicit NativeFixture(const std::string& model) {
        io.start();
        cfg.set("ai.ninfer_native.model", model);
        cfg.set("ai.ninfer_native.kv_tokens", "8192");
        cfg.set("ai.ninfer_native.prefill_chunk", "1024");
        // 投机解码档位可由环境变量覆盖，便于 A/B（引擎启动期固定，改一次要重载）
        const auto envInt = [](const char* k, int def) {
            const char* v = std::getenv(k);
            return (v != nullptr && *v != '\0') ? std::atoi(v) : def;
        };
        cfg.set("ai.ninfer_native.spec", std::to_string(envInt("ZENITH_TEST_NINFER_NATIVE_SPEC", 1)));
        cfg.set("ai.ninfer_native.draft_tokens",
                std::to_string(envInt("ZENITH_TEST_NINFER_NATIVE_DRAFT", 4)));
        cfg.set("ai.ninfer_native.kv_tokens",
                std::to_string(envInt("ZENITH_TEST_NINFER_NATIVE_KV", 8192)));
        cfg.set("ai.ninfer_native.max_context",
                std::to_string(envInt("ZENITH_TEST_NINFER_NATIVE_CTX", 8192)));

        mgr = std::make_unique<plugin::PluginManager>(registry, &pool, &cfg);
        // 原生插件单独目录：550MB MSVC/CUDA 模块不与示例插件混放
        auto loaded = mgr->loadDirectory(ZENITH_TEST_PLUGIN_DIR);
        if (loaded.isErr()) {
            std::fprintf(stderr, "[debug] loadDirectory: %s\n", loaded.error().message.c_str());
        }
        void* table = registry.queryRaw("zenith.ai.INinferCApi", 1);
        service = ai::wrapNinferCAbiService(table, "bonsai2-27b", model);
        if (service == nullptr) return;

        ai::OpenAiApiOptions opt;
        opt.pool = &pool;
        ai::mountOpenAiApi(server.router(), *service, opt);
        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }

    ~NativeFixture() {
        loop.stop();
        if (runner.joinable()) runner.join();
        io.stop();
        service.reset();
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
        return http::HttpClient::fetchBlocking(rq);
    }

    core::Result<http::HttpResponse> get(const std::string& path) {
        http::ClientRequest rq;
        rq.url = base() + path;
        rq.method = http::HttpMethod::Get;
        return http::HttpClient::fetchBlocking(rq);
    }
};

} // namespace

ZTEST(AiNinferNative, InProcessChat) {
    if (!pluginPresent()) {
        std::printf("[ SKIP ] zenith_ai_ninfer_native.dll 未构建\n");
        return;
    }
    const std::string model = resolveModel();
    if (model.empty()) {
        std::printf("[ SKIP ] no .ninfer model (set ZENITH_TEST_NINFER_NATIVE_MODEL)\n");
        return;
    }
    std::printf("[ native ] model = %s\n", model.c_str());

    const auto t0 = std::chrono::steady_clock::now();
    NativeFixture f(model);
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    ZEXPECT_TRUE(f.service != nullptr);
    if (f.service == nullptr) {
        std::printf("[ native ] 插件未注册 C 服务表（DLL 未加载？）\n");
        return;
    }
    std::printf("[ native ] 引擎+插件就绪 %lld ms, backend=%s\n",
                static_cast<long long>(loadMs), f.service->backend().c_str());
    ZEXPECT_TRUE(f.service->backend().find("in-process") != std::string::npos);

    // 进程内引擎的运行态（supervised=false 说明没有子进程）
    const ai::EngineState st = f.service->engineState();
    ZEXPECT_TRUE(st.running);
    ZEXPECT_FALSE(st.supervised);

    auto models = f.get("/v1/models");
    ZEXPECT_TRUE(models.isOk());
    if (models.isOk()) std::printf("[ native ] models: %s\n", models.value().body.c_str());

    const std::string body =
        R"({"messages":[{"role":"user","content":"用一句话介绍你自己"}],"max_tokens":64})";
    const auto g0 = std::chrono::steady_clock::now();
    auto res = f.post("/v1/chat/completions", body);
    const auto genMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g0)
                           .count();
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) {
        std::printf("[ native ] 请求失败：%s\n", res.error().message.c_str());
        return;
    }
    ZEXPECT_EQ(res.value().status, 200);
    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const auto* choices = parsed.value()["choices"].asArray();
    ZEXPECT_TRUE(choices != nullptr && !choices->empty());
    if (choices != nullptr && !choices->empty()) {
        const std::string content = (*choices)[0]["message"]["content"].asString();
        std::printf("[ native ] %lld ms，回复 %zu 字节：%.120s\n", static_cast<long long>(genMs),
                    content.size(), content.c_str());
        // 思考模型在 max_tokens 很小时可能只有 reasoning，没有正文
        const std::string thinking = (*choices)[0]["message"]["reasoning_content"].asString();
        ZEXPECT_TRUE(!content.empty() || !thinking.empty());
    }

    const ai::EngineState after = f.service->engineState();
    std::printf("[ native ] 引擎计数：requests=%lld draft=%lld/%lld\n",
                static_cast<long long>(after.requests), static_cast<long long>(after.draftAccepted),
                static_cast<long long>(after.draftTotal));
    ZEXPECT_TRUE(after.requests >= 1);
}

// ---------------------------------------------------------------------------
//  测速：进程内直调服务（不经 HTTP），读取 ChatResult.stats 里的引擎 timings
//  输入 = prefill（读 prompt），输出 = decode（逐 token 生成）
// ---------------------------------------------------------------------------
ZTEST(AiNinferNative, MeasurePrefillDecode) {
    if (!pluginPresent()) {
        std::printf("[ SKIP ] zenith_ai_ninfer_native.dll 未构建\n");
        return;
    }
    const std::string model = resolveModel();
    if (model.empty()) {
        std::printf("[ SKIP ] no .ninfer model\n");
        return;
    }
    NativeFixture f(model);
    if (f.service == nullptr) return;

    // 该包教程 §0.2 的「数数字」语料：1..300 + 指令，约 1.1K token 输入
    std::string prompt;
    for (int i = 1; i <= 300; ++i) {
        prompt += std::to_string(i);
        prompt += ' ';
    }
    prompt += "\n\nContinue the same number sequence from the next number. Output numbers only, "
              "in the same space-separated format, and keep going until the length limit.";

    // 可用环境变量换成正常推理题（默认用包内教程的数数字语料）
    if (const char* p = std::getenv("ZENITH_TEST_NINFER_NATIVE_PROMPT");
        p != nullptr && *p != '\0') {
        prompt = p; // 仅 ASCII 安全：MinGW 的 getenv 不做 UTF-8 转换
    }
    // 中文等非 ASCII prompt 走文件（UTF-8 原样读入，绕开进程环境变量的编码转换）
    if (const char* pf = std::getenv("ZENITH_TEST_NINFER_NATIVE_PROMPT_FILE");
        pf != nullptr && *pf != '\0') {
        if (FILE* f = std::fopen(pf, "rb")) {
            std::string loaded;
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) loaded.append(buf, n);
            std::fclose(f);
            if (!loaded.empty()) prompt = loaded;
        }
    }
    ai::ChatRequest req;
    req.messages.push_back(ai::ChatMessage{"user", prompt, ""});
    const auto envInt2 = [](const char* k, int def) {
        const char* v = std::getenv(k);
        return (v != nullptr && *v != '\0') ? std::atoi(v) : def;
    };
    req.maxTokens = envInt2("ZENITH_TEST_NINFER_NATIVE_MAXTOK", 1000);
    req.temperature = 0.0;
    // 思考开关：-1 关 / 1 开（引擎默认 0）。开启后 token 全先花在 reasoning 上，
    // 所以要看 reasoning 字节数，并给足 max_tokens 才能出正文。
    req.thinking = envInt2("ZENITH_TEST_NINFER_NATIVE_THINK", -1);

    const auto t0 = std::chrono::steady_clock::now();
    const ai::ChatResult r = f.service->chat(req, nullptr, nullptr);
    const auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();

    if (!r.ok) std::printf("[ measure ] 失败原因：%s\n", r.error.c_str());
    ZEXPECT_TRUE(r.ok);
    std::printf("[ measure ] ok=%d 墙钟=%lld ms 正文=%zu 字节 思考=%zu 字节 finish=%s\n",
                r.ok ? 1 : 0, static_cast<long long>(wallMs), r.content.size(),
                r.reasoning.size(), r.finishReason.c_str());
    std::printf("[ measure ] 输入 %d token / 输出 %d token（思考 %d）\n", r.usage.promptTokens,
                r.usage.completionTokens, 0);
    if (r.stats.valid) {
        std::printf("[ measure ] 输入(prefill) %.1f tok/s · 输出(decode) %.1f tok/s · "
                    "投机 %lld/%lld · 缓存命中 %lld\n",
                    r.stats.prefillTps, r.stats.decodeTps,
                    static_cast<long long>(r.stats.draftAccepted),
                    static_cast<long long>(r.stats.draftTotal),
                    static_cast<long long>(r.stats.cachedTokens));
    } else {
        std::printf("[ measure ] stats.valid=false（引擎未返回 timings）\n");
    }
    if (!r.content.empty()) {
        std::printf("[ measure ] 前 60 字符：%.60s\n", r.content.c_str());
    }
    ZEXPECT_TRUE(r.usage.promptTokens > 0);
    ZEXPECT_TRUE(r.usage.completionTokens > 0);
    ZEXPECT_TRUE(r.stats.valid);
}

ZTEST_MAIN
