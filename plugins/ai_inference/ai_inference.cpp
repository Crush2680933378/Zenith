// ============================================================================
//  ai_inference —— C ABI 插件：LLM 推理网关（M8.1，2026-10 重写）
//
//  三种后端形态（配置 ai.inference.backend）：
//    engine  本地引擎：由本插件**拉起并监管**进程（NInfer / llama-server 等），
//            走 OpenAI 协议对接。引擎的 KVMem / 三元量化 / 投机解码 / 视觉
//            全部保留在进程内，插件只负责生命周期 + 遥测。
//    openai  远端/外部 OpenAI 兼容服务（vLLM / 云端 API / 已在跑的引擎）。
//    mock    确定性后端（无模型也能跑通契约，CI 用）。
//
//  为什么不做进程内 llama.cpp：两套发行版都只有二进制（无 llama.h / 导入库）；
//  本机源码树 b10603 与二进制 b10709 版本不一致；KV 均值中心只在 server 层。
//  详见 docs/PLAN_M8.md 的 M8.1 状态块。
//
//  配置键：
//    ai.inference.backend            auto|engine|openai|mock（默认 auto）
//    ai.inference.engine.exe         引擎可执行文件（全路径）
//    ai.inference.engine.model       模型文件路径，用于展开 <MODEL> 占位
//    ai.inference.engine.args        参数串（支持双引号），占位 <MODEL>/<ENGINE_DIR>
//    ai.inference.engine.env         "K=V;K2=V2"
//    ai.inference.engine.workdir     工作目录（默认引擎所在目录：它从自己目录加载 DLL）
//    ai.inference.engine.ready_line  日志就绪标记（默认 "listening on"）
//    ai.inference.engine.startup_timeout_ms  默认 180000
//    ai.inference.upstream_url       引擎地址；留空则从 args 的 --host/--port 推断
//    ai.inference.upstream_api_key   Bearer 令牌（引擎关闭鉴权时留空）
//    ai.inference.upstream_timeout_ms 默认 600000（长上下文预填很慢）
//    ai.inference.default_model      模型 id（也用作 OpenAI 请求的默认 model）
// ============================================================================
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zenith/abi/zenith_abi.h"

#include "backend.hpp"
#include "engine_supervisor.hpp"

namespace {

const zenith_host_api* g_host = nullptr;

void logAt(zenith_log_level level, const std::string& m) {
    if (g_host != nullptr && g_host->log_write != nullptr) {
        g_host->log_write(g_host->log_ctx, level, "ai_inference", 0, m.c_str());
    }
}
void logInfo(const std::string& m) { logAt(ZENITH_LOG_INFO, m); }
void logError(const std::string& m) { logAt(ZENITH_LOG_ERROR, m); }

std::string cfgStr(const char* key, const char* fallback) {
    const char* v = nullptr;
    if (g_host != nullptr && g_host->config_get != nullptr &&
        g_host->config_get(g_host->config_ctx, key, "", &v) == ZENITH_OK && v != nullptr) {
        return std::string(v);
    }
    return std::string(fallback);
}

int cfgInt(const char* key, int fallback) {
    const std::string s = cfgStr(key, "");
    return s.empty() ? fallback : std::atoi(s.c_str());
}

std::string trimCopy(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

/// 参数串 → argv（双引号包裹的路径可含空格）
std::vector<std::string> splitArgs(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    bool has = false;
    for (char c : s) {
        if (c == '"') {
            inQuote = !inQuote;
            has = true;
            continue;
        }
        if (!inQuote && (c == ' ' || c == '\t')) {
            if (has) {
                out.push_back(cur);
                cur.clear();
                has = false;
            }
            continue;
        }
        cur += c;
        has = true;
    }
    if (has) out.push_back(cur);
    return out;
}

/// "K=V;K2=V2" → [(K,V)]
std::vector<std::pair<std::string, std::string>> splitEnv(const std::string& s) {
    std::vector<std::pair<std::string, std::string>> out;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(';', pos);
        if (end == std::string::npos) end = s.size();
        const std::string item = trimCopy(s.substr(pos, end - pos));
        pos = end + 1;
        if (item.empty()) continue;
        const size_t eq = item.find('=');
        if (eq == std::string::npos) continue;
        out.emplace_back(trimCopy(item.substr(0, eq)), trimCopy(item.substr(eq + 1)));
    }
    return out;
}

std::string dirOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

/// <MODEL> / <ENGINE_DIR> 占位展开
std::string expand(const std::string& in, const std::string& model, const std::string& engineDir) {
    std::string s = in;
    const auto replaceAll = [](std::string& t, const std::string& from, const std::string& to) {
        if (from.empty()) return;
        size_t pos = 0;
        while ((pos = t.find(from, pos)) != std::string::npos) {
            t.replace(pos, from.size(), to);
            pos += to.size();
        }
    };
    replaceAll(s, "<MODEL>", model);
    replaceAll(s, "<ENGINE_DIR>", engineDir);
    return s;
}

/// 从 "--host 127.0.0.1 --port 8091" 拼出 http://host:port
std::string deriveUrl(const std::vector<std::string>& args) {
    std::string host = "127.0.0.1";
    int port = 0;
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--host") host = args[i + 1];
        else if (args[i] == "--port") port = std::atoi(args[i + 1].c_str());
    }
    if (port <= 0) return std::string();
    if (host == "0.0.0.0" || host.empty()) host = "127.0.0.1";
    return "http://" + host + ":" + std::to_string(port);
}

/// 从参数里取 "--model-id X"
std::string modelIdFromArgs(const std::vector<std::string>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--model-id") return args[i + 1];
    }
    return std::string();
}

// ---------------------------------------------------------------------------
//  服务实现：监管器（可选）+ 推理后端
// ---------------------------------------------------------------------------
class InferenceImpl final : public zenith::ai::IInferenceService {
public:
    InferenceImpl(std::unique_ptr<zenith::ai::detail::EngineSupervisor> supervisor,
                  std::unique_ptr<zenith::ai::detail::Backend> backend)
        : supervisor_(std::move(supervisor)), backend_(std::move(backend)) {}

    ~InferenceImpl() override {
        if (supervisor_ != nullptr) supervisor_->stop(); // 先停引擎，再放后端
    }

    [[nodiscard]] std::vector<zenith::ai::ModelInfo> models() override {
        return backend_->models();
    }

    zenith::ai::ChatResult chat(const zenith::ai::ChatRequest& req,
                                const zenith::ai::TokenCallback& onToken,
                                const zenith::ai::TokenCallback& onReasoning) override {
        return backend_->chat(req, onToken, onReasoning);
    }

    zenith::ai::EmbeddingResult embeddings(const std::string& model,
                                           const std::string& input) override {
        return backend_->embeddings(model, input);
    }

    [[nodiscard]] std::string backend() const override { return backend_->name(); }

    [[nodiscard]] zenith::ai::EngineState engineState() const override {
        if (supervisor_ == nullptr) return zenith::ai::EngineState{};
        return supervisor_->snapshot();
    }

private:
    std::unique_ptr<zenith::ai::detail::EngineSupervisor> supervisor_;
    std::unique_ptr<zenith::ai::detail::Backend> backend_;
};

void destroyService(void* p) { delete static_cast<InferenceImpl*>(p); }

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    const std::string kind = cfgStr("ai.inference.backend", "auto");
    const std::string engineExe = cfgStr("ai.inference.engine.exe", "");
    const std::string engineModel = cfgStr("ai.inference.engine.model", "");
    const std::string engineArgs = cfgStr("ai.inference.engine.args", "");
    const std::string engineEnv = cfgStr("ai.inference.engine.env", "");
    const std::string engineWorkDir = cfgStr("ai.inference.engine.workdir", "");
    const std::string readyLine = cfgStr("ai.inference.engine.ready_line", "listening on");
    const int startupTimeoutMs = cfgInt("ai.inference.engine.startup_timeout_ms", 180000);

    std::string upstreamUrl = cfgStr("ai.inference.upstream_url", "");
    const std::string upstreamKey = cfgStr("ai.inference.upstream_api_key", "");
    const int upstreamTimeout = cfgInt("ai.inference.upstream_timeout_ms", 600000);
    std::string modelId = cfgStr("ai.inference.default_model", "");

    std::unique_ptr<zenith::ai::detail::EngineSupervisor> supervisor;
    std::unique_ptr<zenith::ai::detail::Backend> backend;

    const bool wantEngine = (kind == "engine") || (kind == "auto" && !engineExe.empty());

    if (wantEngine) {
        if (engineExe.empty()) {
            logError("ai_inference: backend=engine but ai.inference.engine.exe is empty");
        } else {
            const std::string engineDir = dirOf(engineExe);
            std::vector<std::string> argv = splitArgs(engineArgs);
            for (auto& a : argv) a = expand(a, engineModel, engineDir);

            if (upstreamUrl.empty()) upstreamUrl = deriveUrl(argv);
            if (modelId.empty()) modelId = modelIdFromArgs(argv);

            zenith::ai::detail::EngineSpec spec;
            spec.exe = engineExe;
            spec.args = std::move(argv);
            spec.env = splitEnv(engineEnv);
            spec.workDir = engineWorkDir.empty() ? engineDir : engineWorkDir;
            spec.readyLine = readyLine;
            spec.startupTimeoutMs = startupTimeoutMs;

            supervisor = std::make_unique<zenith::ai::detail::EngineSupervisor>();
            std::string err;
            if (supervisor->start(spec, err)) {
                const auto st = supervisor->snapshot();
                logInfo("ai_inference: engine started (pid=" + std::to_string(st.pid) + ", last=" +
                        st.lastLine + ")");
            } else {
                // 保留监管器：/zenith/ai/status 里能看到 supervised=true、running=false
                // 与失败原因（lastLine），比静默降级更好排查
                logError("ai_inference: engine failed to start: " + err);
            }
        }
    }

    if (upstreamUrl.empty()) {
        if (kind == "openai" || kind == "engine") {
            logError("ai_inference: no upstream url (set ai.inference.upstream_url "
                     "or pass --host/--port in engine.args)");
        }
    } else {
        backend = zenith::ai::detail::makeOpenAiUpstreamBackend(upstreamUrl, upstreamKey, modelId,
                                                                upstreamTimeout);
        if (backend == nullptr) logError("ai_inference: failed to create upstream backend");
    }

    if (backend == nullptr) {
        backend = zenith::ai::detail::makeMockBackend(
            modelId.empty() ? "zenith-mock-1" : modelId, cfgInt("ai.inference.embedding_dim", 64));
    }
    if (backend == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;

    auto* svc = new (std::nothrow) InferenceImpl(std::move(supervisor), std::move(backend));
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;

    const auto loadedModels = svc->models();
    logInfo("ai_inference: ready (backend=" + svc->backend() + ", upstream=" + upstreamUrl +
            ", models=" + std::to_string(loadedModels.size()) + ")");

    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::ai::IInferenceService::service_name(),
                                  zenith::ai::IInferenceService::service_version(),
                                  static_cast<void*>(svc), &destroyService);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_inference",
    "0.2.0",
    "Zenith AI plugin: LLM inference gateway (supervised local engine / OpenAI upstream)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
