// ============================================================================
//  ai_ninfer 插件活体测试（NInfer 引擎：无 C ABI ⇒ 进程监管 + OpenAI HTTP）
//  环境变量：
//    ZENITH_TEST_NINFER_EXE    引擎 exe 全路径（未设置则 SKIP）
//    ZENITH_TEST_NINFER_MODEL  .ninfer 权重路径
//    ZENITH_TEST_NINFER_PORT   引擎端口（默认 8113）
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

using Json = http::json::JsonValue;

std::string envOr(const char* k, const std::string& def) {
    const char* v = std::getenv(k);
    return (v != nullptr && *v != '\0') ? std::string(v) : def;
}

struct NinferFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    core::KeyValueConfig cfg;
    concurrency::ThreadPool pool{2};
    plugin::ServiceRegistry registry;
    std::unique_ptr<plugin::PluginManager> mgr;
    ai::IInferenceService* service = nullptr;
    std::thread runner;

    explicit NinferFixture(const std::string& exe, const std::string& model,
                           const std::string& port) {
        io.start();
        cfg.set("ai.ninfer.exe", exe);
        cfg.set("ai.ninfer.model", model);
        cfg.set("ai.ninfer.model_id", "qwen3.8-27b");
        cfg.set("ai.ninfer.args",
                "<MODEL> --host 127.0.0.1 --port " + port +
                    " --model-id qwen3.8-27b --max-context 32768 --kv-capacity 8192"
                    " --kv-dtype k8v4 --host-kv-mib 4096 --prefill-chunk 1024 --spec mtp"
                    " --draft-tokens 4 --default-max-tokens 32768"
                    " --default-reasoning-effort none --max-concurrency 1"
                    " --max-shared-prefixes 0 --presence-penalty 0"
                    " --temperature 0.7 --top-p 0.9 --top-k 20");
        cfg.set("ai.ninfer.env",
                "NINFER_KV_WINDOW=16384;NINFER_KV_RETRIEVE=8192;NINFER_KV_RING=1;"
                "NINFER_HOST_PAGEABLE=1;NINFER_KV_REUSE_HOSTBACKED=1");
        cfg.set("ai.ninfer.timeout_ms", "900000");

        mgr = std::make_unique<plugin::PluginManager>(registry, &pool, &cfg);
        auto loaded = mgr->loadDirectory(ZENITH_TEST_PLUGIN_DIR);
        if (loaded.isErr()) {
            std::fprintf(stderr, "[debug] loadDirectory: %s\n", loaded.error().message.c_str());
        }
        service = static_cast<ai::IInferenceService*>(
            registry.queryRaw("zenith.ai.INinferInference", 1));
        if (service == nullptr) return;

        ai::OpenAiApiOptions opt;
        opt.pool = &pool;
        ai::mountOpenAiApi(server.router(), *service, opt);
        ZEXPECT_TRUE(static_cast<bool>(server.listen("127.0.0.1", 0)));
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }

    ~NinferFixture() {
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

ZTEST(AiNinfer, SupervisesEngineAndServes) {
    const std::string exe = envOr("ZENITH_TEST_NINFER_EXE", "");
    if (exe.empty()) {
        std::printf("[ SKIP ] ZENITH_TEST_NINFER_EXE not set\n");
        return;
    }
    const std::string model = envOr("ZENITH_TEST_NINFER_MODEL", "");
    const std::string port = envOr("ZENITH_TEST_NINFER_PORT", "8113");

    NinferFixture f(exe, model, port);
    ZEXPECT_TRUE(f.service != nullptr);
    if (f.service == nullptr) return;

    // 关键：后端标识必须自带 “no C ABI”，这是本插件与 ai_llama 的核心区别
    std::printf("[ ninfer ] backend = %s\n", f.service->backend().c_str());
    ZEXPECT_TRUE(f.service->backend().find("no C ABI") != std::string::npos);

    const ai::EngineState st = f.service->engineState();
    ZEXPECT_TRUE(st.supervised);
    ZEXPECT_TRUE(st.running);
    std::printf("[ ninfer ] pid=%lld last=%s\n", static_cast<long long>(st.pid),
                st.lastLine.c_str());

    auto status = f.get("/zenith/ai/status");
    ZEXPECT_TRUE(status.isOk());
    if (status.isOk()) {
        auto parsed = Json::parse(status.value().body);
        ZEXPECT_TRUE(parsed.isOk());
        if (parsed.isOk()) {
            ZEXPECT_TRUE(parsed.value()["engine"]["running"].asBool(false));
            std::printf("[ ninfer ] status: %s\n", status.value().body.substr(0, 200).c_str());
        }
    }

    // 一条短请求（引擎 --default-reasoning-effort none，直接出正文）
    auto res = f.post("/v1/chat/completions",
                      R"({"messages":[{"role":"user","content":"用一句话介绍你自己"}],"max_tokens":64})");
    ZEXPECT_TRUE(res.isOk());
    if (res.isErr()) {
        std::printf("[ ninfer ] 请求失败：%s\n", res.error().message.c_str());
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
        std::printf("[ ninfer ] 回复 %zu 字节：%.120s\n", content.size(), content.c_str());
        ZEXPECT_TRUE(!content.empty());
    }
}

ZTEST_MAIN
