// ============================================================================
//  ai_ninfer —— NInfer 引擎专用插件（M8.1，2026-10-21）
//
//  ████████████████████████████████████████████████████████████████████████
//  ★★ 重要：NInfer **不提供 C ABI，无法进程内嵌入** ★★
//
//  实测与取证（2026-10-21）：
//    1. 本机 engine 包（C:\...\infer-engine-sm120a-20261002\engine）只有
//       `ninfer-serve-120a.exe` + 运行时 DLL，**没有任何头文件、没有 .lib/.dll.a**；
//    2. 上游 Neroued/ninfer 明确写「The in-tree C++ headers are not distributed
//       as an installed SDK」，且只提供 CLI 与 OpenAI/Anthropic HTTP 接口；
//    3. 其能力（KVMem 分页、MTP/DFlash2 投机解码、视觉驻留）全在 server 进程内，
//       连 llama.cpp 那样的 llama.h 语义都没有。
//
//  ⇒ 因此本插件**不是**“库插件”，而是「进程监管 + OpenAI 协议代理」：
//     拉起 ninfer-serve-*.exe → 等 `listening on` 就绪 → 用 HTTP 说 OpenAI 协议
//     → 卸载时回收进程（Windows Job 对象，宿主崩了也不留孤儿）。
//     代价：每次请求多一跳 loopback HTTP；收益：拿到引擎全部原生能力。
//     若将来上游发布 C ABI 头+导入库，才可能改回进程内。
//  ████████████████████████████████████████████████████████████████████████
//
//  配置键（宿主 config_get）：
//    ai.ninfer.exe        ninfer-serve-*.exe 全路径（必填）
//    ai.ninfer.model      .ninfer 权重路径，展开 <MODEL> 占位
//    ai.ninfer.args       参数串，支持 <MODEL> / <ENGINE_DIR>
//    ai.ninfer.env        "K=V;K2=V2"（KVMem 变量放这里）
//    ai.ninfer.workdir    工作目录（默认 exe 所在目录：引擎从自身目录加载 DLL）
//    ai.ninfer.ready_line 日志就绪标记（默认 "listening on"）
//    ai.ninfer.startup_timeout_ms  默认 180000
//    ai.ninfer.url        引擎地址；留空则从 args 的 --host/--port 推断
//    ai.ninfer.api_key    Bearer 令牌（引擎关闭鉴权时留空）
//    ai.ninfer.model_id   对外模型 id（默认从 --model-id 推断）
//    ai.ninfer.timeout_ms 单请求超时（默认 900000：长上下文预填很慢）
// ============================================================================
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zenith/abi/zenith_abi.h"
#include "zenith/ai/inference_service.hpp"

#include "backend.hpp"          // 来自 plugins/ai_inference
#include "engine_supervisor.hpp" // 来自 plugins/ai_inference

namespace {

const zenith_host_api* g_host = nullptr;

/// 在日志里反复出现的 ABI 说明（也用于 /zenith/ai/status 的 backend 字段）
constexpr const char* kAbiNote =
    "NInfer 无 C ABI：引擎以独立进程运行，插件经 OpenAI HTTP 协议对接（不可进程内嵌入）";

void logAt(zenith_log_level level, const std::string& m) {
    if (g_host != nullptr && g_host->log_write != nullptr) {
        g_host->log_write(g_host->log_ctx, level, "ai_ninfer", 0, m.c_str());
    }
}
void logInfo(const std::string& m) { logAt(ZENITH_LOG_INFO, m); }
void logWarn(const std::string& m) { logAt(ZENITH_LOG_WARN, m); }
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

std::string modelIdFromArgs(const std::vector<std::string>& args) {
    for (size_t i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--model-id") return args[i + 1];
    }
    return std::string();
}

// ---------------------------------------------------------------------------
//  服务实现：监管器 + OpenAI 上游后端（**没有**任何 NInfer 头文件/库）
// ---------------------------------------------------------------------------
class NinferService final : public zenith::ai::IInferenceService {
public:
    NinferService(std::unique_ptr<zenith::ai::detail::EngineSupervisor> supervisor,
                  std::unique_ptr<zenith::ai::detail::Backend> backend)
        : supervisor_(std::move(supervisor)), backend_(std::move(backend)) {}

    ~NinferService() override {
        if (supervisor_ != nullptr) supervisor_->stop(); // 先收进程，再放后端
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

    /// 后端标识里直接写明“无 C ABI”，避免运维误以为这是库插件
    [[nodiscard]] std::string backend() const override { return "ninfer-http (no C ABI)"; }

    [[nodiscard]] zenith::ai::EngineState engineState() const override {
        if (supervisor_ == nullptr) return zenith::ai::EngineState{};
        return supervisor_->snapshot();
    }

private:
    std::unique_ptr<zenith::ai::detail::EngineSupervisor> supervisor_;
    std::unique_ptr<zenith::ai::detail::Backend> backend_;
};

void destroyService(void* p) { delete static_cast<NinferService*>(p); }

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    // 先把“无 C ABI”这件事说清楚，再干活
    logWarn(std::string("ai_ninfer: ") + kAbiNote);

    const std::string exe = cfgStr("ai.ninfer.exe", "");
    if (exe.empty()) {
        logError("ai_ninfer: ai.ninfer.exe is empty");
        return ZENITH_ERR_INVALID_ARGUMENT;
    }

    const std::string engineModel = cfgStr("ai.ninfer.model", "");
    const std::string engineDir = dirOf(exe);
    std::vector<std::string> argv = splitArgs(cfgStr("ai.ninfer.args", ""));
    for (auto& a : argv) a = expand(a, engineModel, engineDir);

    std::string url = cfgStr("ai.ninfer.url", "");
    if (url.empty()) url = deriveUrl(argv);
    std::string modelId = cfgStr("ai.ninfer.model_id", "");
    if (modelId.empty()) modelId = modelIdFromArgs(argv);
    if (url.empty()) {
        logError("ai_ninfer: cannot resolve engine url (set ai.ninfer.url or --host/--port)");
        return ZENITH_ERR_INVALID_ARGUMENT;
    }

    zenith::ai::detail::EngineSpec spec;
    spec.exe = exe;
    spec.args = std::move(argv);
    spec.env = splitEnv(cfgStr("ai.ninfer.env", ""));
    const std::string workDir = cfgStr("ai.ninfer.workdir", "");
    spec.workDir = workDir.empty() ? engineDir : workDir;
    spec.readyLine = cfgStr("ai.ninfer.ready_line", "listening on");
    spec.startupTimeoutMs = cfgInt("ai.ninfer.startup_timeout_ms", 180000);

    auto supervisor = std::make_unique<zenith::ai::detail::EngineSupervisor>();
    std::string err;
    if (!supervisor->start(spec, err)) {
        // 保留监管器：status 里能看到 supervised=true / running=false / 失败原因
        logError("ai_ninfer: engine failed to start: " + err);
    } else {
        const auto st = supervisor->snapshot();
        logInfo("ai_ninfer: engine up (pid=" + std::to_string(st.pid) + ", url=" + url +
                ", last=" + st.lastLine + ")");
    }

    auto backend = zenith::ai::detail::makeOpenAiUpstreamBackend(
        url, cfgStr("ai.ninfer.api_key", ""), modelId, cfgInt("ai.ninfer.timeout_ms", 900000));

    auto* svc = new (std::nothrow) NinferService(std::move(supervisor), std::move(backend));
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;

    // 独立服务名：可与 ai_llama / ai_inference 同目录共存
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  "zenith.ai.INinferInference", 1, static_cast<void*>(svc),
                                  &destroyService);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_ninfer",
    "0.1.0",
    "Zenith AI plugin: NInfer engine supervisor + OpenAI HTTP adapter (NInfer has NO C ABI)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
