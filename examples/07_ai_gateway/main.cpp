// ============================================================================
//  07_ai_gateway —— 把 AI 推理插件挂成 OpenAI 兼容网关（ZenithDesk 的后端）
//
//  自动挑选已加载的推理服务（按优先级），挂载：
//    GET  /v1/models            OpenAI 模型列表
//    POST /v1/chat/completions  （stream=true 走 SSE）
//    POST /v1/completions / /v1/embeddings
//    GET  /zenith/ai/status     网关 + 引擎运行态（插件/引擎/计数器）
//    GET  /metrics              Prometheus 文本格式
//    GET  /healthz              存活探针
//
//  用法：ai_gateway [端口] [插件目录]
//  默认：端口 8090，插件目录取构建期 ZENITH_EXAMPLE_PLUGIN_DIR。
//  插件默认落 mock 后端（无模型也能跑通全链路），换模型只需改插件配置。
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>

#include "zenith/ai/inference_service.hpp"
#include "zenith/ai/openai_api.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/metrics/metrics.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;

namespace {

/// 按优先级找一个可用的推理服务
ai::IInferenceService* pickService(plugin::ServiceRegistry& registry) {
    const char* names[] = {
        "zenith.ai.IInferenceService",   // 通用网关（mock / 上游 / 引擎监管）
        "zenith.ai.ILlamaInference",     // 进程内 llama.cpp
        "zenith.ai.INinferInference",    // NInfer 子进程
    };
    for (const char* n : names) {
        void* raw = registry.queryRaw(n, 1);
        if (raw != nullptr) {
            std::printf("[gateway] 使用推理服务: %s\n", n);
            return static_cast<ai::IInferenceService*>(raw);
        }
    }
    return nullptr;
}

} // namespace

int main(int argc, char** argv) {
    const uint16_t port =
        argc > 1 ? static_cast<uint16_t>(std::atoi(argv[1])) : static_cast<uint16_t>(8090);
#ifdef ZENITH_EXAMPLE_PLUGIN_DIR
    const std::string pluginDir = argc > 2 ? argv[2] : ZENITH_EXAMPLE_PLUGIN_DIR;
#else
    const std::string pluginDir = argc > 2 ? argv[2] : "./plugins";
#endif

    core::KeyValueConfig cfg;
    cfg.set("ai.inference.backend", "mock"); // 示例默认确定性后端；换真模型改这里
    cfg.set("ai.inference.default_model", "zenith-mock-1");
    cfg.set("ai.inference.embedding_dim", "64");

    concurrency::ThreadPool pool{4};
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, &cfg);

    auto loaded = mgr.loadDirectory(pluginDir);
    if (loaded.isErr()) {
        std::fprintf(stderr, "[gateway] 插件目录不可用: %s\n", loaded.error().message.c_str());
        return 1;
    }
    std::printf("[gateway] 已加载插件 %zu 个（目录 %s）\n", loaded.value(), pluginDir.c_str());
    (void)mgr.startAll();

    ai::IInferenceService* svc = pickService(registry);
    if (svc == nullptr) {
        std::fprintf(stderr, "[gateway] 没有可用的推理服务，退出\n");
        return 2;
    }

    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    io.start();
    http::HttpServer server{&loop, &io};

    auto& r = server.router();

    // QPS / 错误 / 在途 / 延迟直方图自动打点（GUI 的 HTTP 面板读这些）
    r.use(http::metricsMiddleware());
    r.use(http::requestIdMiddleware());
    // 允许浏览器/前端直连（GUI 走主进程 fetch，本身不受 CORS 限制；此处便于浏览器调试）
    r.use([](http::Context& c) {
        c.setHeader("Access-Control-Allow-Origin", "*");
        c.setHeader("Access-Control-Allow-Headers", "Content-Type, Authorization");
        c.setHeader("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        if (c.method() == http::HttpMethod::Options) {
            c.status(204).response().body.clear();
            return;
        }
        c.next();
    });

    // 健康探针
    r.get("/healthz", [](http::Context& c) {
        http::json::JsonValue v = http::json::JsonValue::object();
        v["status"] = "ok";
        v["uptime_ms"] = metrics::processUptimeMs();
        c.json(200, v);
    });

    // Prometheus 指标（GUI 用来画速率/令牌数）
    r.get("/metrics", [](http::Context& c) {
        c.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        c.response().body = metrics::Registry::instance().exportPrometheus();
    });

    // OpenAI 兼容面 + /zenith/ai/status
    ai::OpenAiApiOptions opt;
    opt.pool = &pool;
    ai::mountOpenAiApi(r, *svc, opt);


    if (!server.listen("127.0.0.1", port)) {
        std::fprintf(stderr, "[gateway] 监听 %u 失败\n", static_cast<unsigned>(port));
        return 3;
    }

    std::printf("[gateway] 就绪 http://127.0.0.1:%u   backend=%s  model=%s\n",
                static_cast<unsigned>(port), svc->backend().c_str(),
                svc->models().empty() ? "?" : svc->models().front().id.c_str());
    std::printf("[gateway] 端点: /v1/models · /v1/chat/completions · /zenith/ai/status · "
                "/metrics · /healthz\n");

    loop.loop();

    io.stop();
    mgr.unloadAll();
    return 0;
}
