// ============================================================================
//  07_ai_gateway —— 把 AI 推理插件挂成 OpenAI 兼容网关（ZenithDesk 的后端）
//
//  支持真模型推理。用 --model 指一个本地权重，网关按扩展名自动选插件：
//    *.gguf   → ai_llama 插件（进程内 llama.cpp，纯 C ABI）
//    *.ninfer → ai_ninfer_native（进程内，自研 C ABI）或 ai_ninfer（子进程）
//    不给 --model → mock（确定性假后端，用于联调界面）
//
//  用法：
//    ai_gateway                                    # 8090 + mock
//    ai_gateway 8090 "" --model D:\m\x.gguf --ctx 4096 --threads 8
//    ai_gateway 8090 <插件目录> --model x.ninfer --backend ninfer-native
//    ai_gateway 8090 "" --backend openai --upstream https://api.deepseek.com/v1
//
//  端点：/v1/{models,chat/completions,completions,embeddings} · /zenith/ai/status
//        /metrics · /healthz
// ============================================================================
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zenith/ai/inference_service.hpp"
#include "zenith/ai/ninfer_c_api_client.hpp"
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

namespace fs = std::filesystem;

struct Options {
    uint16_t port = 8090;
    std::string pluginDir;
    std::string model;             ///< 本地权重路径（.gguf / .ninfer）
    std::string backend = "auto";  ///< auto|llama|ninfer|ninfer-native|openai|mock
    std::string upstream;          ///< --backend openai 时的 base url
    std::string engineExe;         ///< --backend ninfer 时的引擎可执行文件
    int ctx = 4096;
    int threads = 0;
    int gpuLayers = 0;
    int enginePort = 8123;         ///< 子进程引擎监听端口
    int spec = 0;                  ///< 0 用插件默认(MTP)，1 MTP，2 DFlash，3 DFlash2
    int draft = 0;                 ///< 0 用插件默认(4)；DFlash2 建议 12
};

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extOf(const std::string& p) { return lower(fs::path(p).extension().string()); }

std::string stemOf(const std::string& p) {
    const std::string s = fs::path(p).stem().string();
    return s.empty() ? std::string("local-model") : s;
}

/// 极简命令行解析：位置参数 [端口] [插件目录] + --k v
Options parseArgs(int argc, char** argv) {
    Options o;
#ifdef ZENITH_EXAMPLE_PLUGIN_DIR
    o.pluginDir = ZENITH_EXAMPLE_PLUGIN_DIR;
#else
    o.pluginDir = "./plugins";
#endif
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return (i + 1 < argc) ? argv[++i] : std::string(); };
        if (a == "--model") {
            o.model = next();
        } else if (a == "--backend") {
            o.backend = lower(next());
        } else if (a == "--upstream") {
            o.upstream = next();
        } else if (a == "--engine") {
            o.engineExe = next();
        } else if (a == "--ctx") {
            o.ctx = std::atoi(next().c_str());
        } else if (a == "--threads") {
            o.threads = std::atoi(next().c_str());
        } else if (a == "--gpu-layers") {
            o.gpuLayers = std::atoi(next().c_str());
        } else if (a == "--engine-port") {
            o.enginePort = std::atoi(next().c_str());
        } else if (a == "--spec") {
            o.spec = std::atoi(next().c_str());
        } else if (a == "--draft") {
            o.draft = std::atoi(next().c_str());
        } else if (a.rfind("--", 0) == 0) {
            std::fprintf(stderr, "[gateway] 忽略未知参数: %s\n", a.c_str());
        } else {
            positional.push_back(a);
        }
    }
    if (!positional.empty() && !positional[0].empty()) {
        o.port = static_cast<uint16_t>(std::atoi(positional[0].c_str()));
    }
    if (positional.size() > 1 && !positional[1].empty()) o.pluginDir = positional[1];
    return o;
}

/// auto：按权重扩展名推断插件
std::string resolveBackend(const Options& o) {
    if (o.backend != "auto") return o.backend;
    if (!o.model.empty()) {
        const std::string e = extOf(o.model);
        if (e == ".gguf") return "llama";
        if (e == ".ninfer") return "ninfer-native";
    }
    return "mock";
}

/// 加载插件前写入各插件要读的配置
void applyBackendConfig(core::KeyValueConfig& cfg, const Options& o, const std::string& backend) {
    if (backend == "mock") {
        cfg.set("ai.inference.backend", "mock");
        cfg.set("ai.inference.default_model", "zenith-mock-1");
        cfg.set("ai.inference.embedding_dim", "64");
        return;
    }
    if (backend == "llama") {
        cfg.set("ai.llama.model", o.model);
        cfg.set("ai.llama.model_id", stemOf(o.model));
        cfg.set("ai.llama.context", std::to_string(o.ctx));
        cfg.set("ai.llama.gpu_layers", std::to_string(o.gpuLayers));
        if (o.threads > 0) cfg.set("ai.llama.threads", std::to_string(o.threads));
        return;
    }
    if (backend == "ninfer" || backend == "ninfer-native") {
        if (!o.engineExe.empty()) cfg.set("ai.ninfer.exe", o.engineExe);
        cfg.set("ai.ninfer.model", o.model);
        cfg.set("ai.ninfer.model_id", stemOf(o.model));
        cfg.set("ai.ninfer.url", "http://127.0.0.1:" + std::to_string(o.enginePort));
        cfg.set("ai.inference.engine.exe", o.engineExe);
        cfg.set("ai.inference.engine.model", o.model);
        cfg.set("ai.inference.backend", "engine");
        // 原生插件读的是自己的键；不设它 init 会因为拿不到权重路径而失败
        cfg.set("ai.ninfer_native.model", o.model);
        cfg.set("ai.ninfer_native.max_context", std::to_string(o.ctx));
        cfg.set("ai.ninfer_native.kv_tokens", std::to_string(o.ctx));
        if (o.spec > 0) cfg.set("ai.ninfer_native.spec", std::to_string(o.spec));
        if (o.draft > 0) cfg.set("ai.ninfer_native.draft_tokens", std::to_string(o.draft));
        return;
    }
    if (backend == "openai") {
        cfg.set("ai.inference.backend", "openai");
        cfg.set("ai.inference.upstream_url", o.upstream);
        cfg.set("ai.inference.default_model", "upstream");
    }
}

struct Picked {
    ai::IInferenceService* service = nullptr;
    std::unique_ptr<ai::IInferenceService> owned; ///< native C ABI 的宿主侧包装
    std::string how;
};

/// 按 backend 挑服务；native C ABI 需宿主包一层成 IInferenceService
Picked pickService(plugin::ServiceRegistry& registry, const std::string& backend, const Options& o) {
    Picked p;
    auto take = [&](const char* name, const char* how) -> bool {
        void* raw = registry.queryRaw(name, 1);
        if (raw == nullptr) return false;
        p.service = static_cast<ai::IInferenceService*>(raw);
        p.how = how;
        return true;
    };

    if (backend == "ninfer-native") {
        if (void* table = registry.queryRaw("zenith.ai.INinferCApi", 1); table != nullptr) {
            p.owned = ai::wrapNinferCAbiService(table, stemOf(o.model), o.model);
            if (p.owned != nullptr) {
                p.service = p.owned.get();
                p.how = "ninfer c-abi（进程内）";
                return p;
            }
        }
    }
    if (backend == "llama" && take("zenith.ai.ILlamaInference", "llama.cpp（进程内）")) return p;
    if (backend == "ninfer" && take("zenith.ai.INinferInference", "ninfer（子进程）")) return p;
    if (take("zenith.ai.IInferenceService", "ai_inference（mock/上游/引擎）")) return p;
    if (take("zenith.ai.ILlamaInference", "llama.cpp（进程内）")) return p;
    if (take("zenith.ai.INinferInference", "ninfer（子进程）")) return p;
    return p;
}

} // namespace

int main(int argc, char** argv) {
    // 逐行刷新：这是常驻服务，输出被重定向到文件时不能因为缓冲丢掉关键日志
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    const Options opt = parseArgs(argc, argv);
    const std::string backend = resolveBackend(opt);

    if (!opt.model.empty() && !fs::exists(opt.model)) {
        std::fprintf(stderr, "[gateway] 模型不存在: %s\n", opt.model.c_str());
        return 4;
    }

    core::KeyValueConfig cfg;
    applyBackendConfig(cfg, opt, backend);

    concurrency::ThreadPool pool{4};
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, &cfg);

    auto loaded = mgr.loadDirectory(opt.pluginDir);
    if (loaded.isErr()) {
        std::fprintf(stderr, "[gateway] 插件目录不可用: %s\n", loaded.error().message.c_str());
        return 1;
    }
    std::printf("[gateway] 插件目录 %s（%zu 个）backend=%s\n", opt.pluginDir.c_str(),
                loaded.value(), backend.c_str());
    (void)mgr.startAll();

    Picked picked = pickService(registry, backend, opt);
    if (picked.service == nullptr) {
        std::fprintf(stderr,
                     "[gateway] 没有可用的推理服务（backend=%s）\n"
                     "          llama 需要 zenith_ai_llama.dll；ninfer 需要 "
                     "zenith_ai_ninfer.dll / zenith_ai_ninfer_native.dll\n",
                     backend.c_str());
        return 2;
    }
    ai::IInferenceService& svc = *picked.service;
    std::printf("[gateway] 推理服务: %s\n", picked.how.c_str());
    std::printf("[gateway] backend=%s  模型=%s\n", svc.backend().c_str(),
                svc.models().empty() ? "?" : svc.models().front().id.c_str());

    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    io.start();
    http::HttpServer server{&loop, &io};
    auto& r = server.router();

    // QPS / 错误 / 在途 / 延迟直方图自动打点（GUI 的 HTTP 面板读这些）
    r.use(http::metricsMiddleware());
    r.use(http::requestIdMiddleware());

    r.get("/healthz", [](http::Context& c) {
        http::json::JsonValue v = http::json::JsonValue::object();
        v["status"] = "ok";
        v["uptime_ms"] = metrics::processUptimeMs();
        c.json(200, v);
    });

    r.get("/metrics", [](http::Context& c) {
        c.setHeader("Content-Type", "text/plain; version=0.0.4; charset=utf-8");
        c.response().body = metrics::Registry::instance().exportPrometheus();
    });

    ai::OpenAiApiOptions apiOpt;
    apiOpt.pool = &pool;
    ai::mountOpenAiApi(r, svc, apiOpt);

    if (!server.listen("127.0.0.1", opt.port)) {
        std::fprintf(stderr, "[gateway] 监听 %u 失败\n", static_cast<unsigned>(opt.port));
        return 3;
    }

    std::printf("[gateway] 就绪 http://127.0.0.1:%u\n", static_cast<unsigned>(opt.port));
    std::printf("[gateway] 端点: /v1/models · /v1/chat/completions · /zenith/ai/status · "
                "/metrics · /healthz\n");

    loop.loop();

    io.stop();
    mgr.unloadAll();
    return 0;
}
