// ============================================================================
//  ai_llama 插件活体测试：进程内 llama.cpp（纯 C ABI）加载真实 GGUF 并生成
//  环境变量：ZENITH_TEST_LLAMA_MODEL（未设置时尝试 D:\App\Qwen3.8-27B-UD-IQ3_XXS.gguf）
//  未找到模型 → SKIP，不影响 CI。
// ============================================================================
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "ztest.hpp"

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

namespace {

using Json = http::json::JsonValue;

/// 插件没被构建（CI 没有 llama.cpp 安装前缀）→ 必须整例跳过，而不是断言失败
bool pluginPresent() {
    std::error_code ec;
    const std::string name = std::string("zenith_ai_llama") + plugin::pluginFileExtension();
    return std::filesystem::exists(std::filesystem::path(ZENITH_TEST_PLUGIN_DIR) / name, ec);
}

std::string resolveModelPath() {
    const char* env = std::getenv("ZENITH_TEST_LLAMA_MODEL");
    if (env != nullptr && *env != '\0') return env;
    const char* kDefault = "D:\\App\\Qwen3.8-27B-UD-IQ3_XXS.gguf";
    FILE* f = std::fopen(kDefault, "rb");
    if (f == nullptr) return std::string();
    std::fclose(f);
    return kDefault;
}

struct LlamaFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    core::KeyValueConfig cfg;
    concurrency::ThreadPool pool{2};
    plugin::ServiceRegistry registry;
    std::unique_ptr<plugin::PluginManager> mgr;
    ai::IInferenceService* service = nullptr;
    std::thread runner;

    explicit LlamaFixture(const std::string& modelPath) {
        io.start();
        cfg.set("ai.llama.model", modelPath);
        cfg.set("ai.llama.model_id", "qwen3.8-27b-iq3xxs");
        cfg.set("ai.llama.context", "2048");
        cfg.set("ai.llama.gpu_layers", "0"); // 本插件链接的是 CPU 后端
        cfg.set("ai.llama.threads", "8");

        mgr = std::make_unique<plugin::PluginManager>(registry, &pool, &cfg);
        auto loaded = mgr->loadDirectory(ZENITH_TEST_PLUGIN_DIR);
        if (loaded.isErr()) {
            std::fprintf(stderr, "[debug] loadDirectory: %s\n", loaded.error().message.c_str());
        }
        void* raw = registry.queryRaw("zenith.ai.ILlamaInference", 1);
        service = static_cast<ai::IInferenceService*>(raw);
        if (service == nullptr) return;

        ai::OpenAiApiOptions opt;
        opt.pool = &pool;
        ai::mountOpenAiApi(server.router(), *service, opt);
        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }

    ~LlamaFixture() {
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

ZTEST(AiLlama, InProcessModelLoadedAndChats) {
    if (!pluginPresent()) {
        std::printf("[ SKIP ] zenith_ai_llama.dll 未构建（缺 llama.cpp 前缀）\n");
        return;
    }
    const std::string model = resolveModelPath();
    if (model.empty()) {
        std::printf("[ SKIP ] no GGUF model (set ZENITH_TEST_LLAMA_MODEL)\n");
        return;
    }
    std::printf("[ llama ] model = %s\n", model.c_str());

    const auto t0 = std::chrono::steady_clock::now();
    LlamaFixture f(model);
    const auto loadMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0)
                            .count();
    ZEXPECT_TRUE(f.service != nullptr);
    if (f.service == nullptr) return;
    std::printf("[ llama ] plugin+model ready in %lld ms, backend=%s\n",
                static_cast<long long>(loadMs), f.service->backend().c_str());
    ZEXPECT_EQ(f.service->backend(), std::string("llama.cpp-inproc"));

    // 模型列表
    auto models = f.get("/v1/models");
    ZEXPECT_TRUE(models.isOk());
    if (models.isOk()) {
        auto parsed = Json::parse(models.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            const auto* data = parsed.value()["data"].asArray();
            ZEXPECT_TRUE(data != nullptr && !data->empty());
            if (data != nullptr && !data->empty()) {
                ZEXPECT_EQ((*data)[0]["id"].asString(), std::string("qwen3.8-27b-iq3xxs"));
                std::printf("[ llama ] /v1/models -> %s\n",
                            parsed.value()["data"].dump().c_str());
            }
        }
    }

    // 一次短生成（贪心，24 token：CPU 跑 27B 也能在十几秒内出结果）
    const std::string body =
        R"({"model":"qwen3.8-27b-iq3xxs","messages":[{"role":"user","content":"Reply with exactly: zenith-ok"}],)"
        R"("max_tokens":24,"temperature":0})";
    const auto g0 = std::chrono::steady_clock::now();
    auto res = f.post("/v1/chat/completions", body);
    const auto genMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - g0)
                           .count();
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) {
        std::printf("[ llama ] 请求失败：%s\n", res.error().message.c_str());
        return;
    }
    ZEXPECT_EQ(res.value().status, 200);
    auto parsed = Json::parse(res.value().body);
    ZEXPECT_TRUE(parsed.isOk());
    if (parsed.isErr()) return;
    const auto* choices = parsed.value()["choices"].asArray();
    ZEXPECT_TRUE(choices != nullptr && !choices->empty());
    if (choices == nullptr || choices->empty()) return;
    const std::string content = (*choices)[0]["message"]["content"].asString();
    std::printf("[ llama ] %lld ms，输出 %zu 字节：%.100s\n", static_cast<long long>(genMs),
                content.size(), content.c_str());
    ZEXPECT_TRUE(!content.empty());
    ZEXPECT_TRUE(parsed.value()["usage"]["prompt_tokens"].asInt() > 0);
    ZEXPECT_TRUE(parsed.value()["usage"]["completion_tokens"].asInt() > 0);
}

ZTEST_MAIN
