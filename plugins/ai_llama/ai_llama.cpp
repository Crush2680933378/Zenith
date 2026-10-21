// ============================================================================
//  ai_llama —— 进程内 llama.cpp 插件（M8.1，2026-10-21）
//
//  ★ C ABI 结论：llama.cpp **提供纯 C ABI**（include/llama.h 全在 extern "C" 内，
//    llama_model / llama_context 都是不透明句柄，LLAMA_API 只用 visibility/
//    dllimport 控制符号导出）。因此可以**进程内静态链接**，不需要子进程、不需要
//    走 HTTP——这正是本插件与 ai_ninfer 的根本区别。
//
//  构建：llama.cpp 由本机源码（C:\APP\GitHubCode\llama.cpp, b10603）用 MinGW 编译
//    成静态库，安装前缀见 CMake 变量 ZENITH_LLAMA_ROOT。CPU 后端（ggml-cpu）。
//    ⇒ n_gpu_layers 对 CPU-only 构建无效；要 GPU 需用 MSVC + CUDA 重编 llama.cpp
//      （同一份源码，接口不变），再改 ZENITH_LLAMA_ROOT 指向新前缀即可。
//
//  配置键（宿主 config_get）：
//    ai.llama.model        .gguf 全路径（必填）
//    ai.llama.model_id     对外模型 id（默认取文件名主干）
//    ai.llama.context      n_ctx（默认 4096）
//    ai.llama.gpu_layers   n_gpu_layers（默认 0，CPU-only 构建忽略）
//    ai.llama.threads      n_threads（默认 hardware_concurrency/2）
//    ai.llama.flash_attn   1 = 强制开启 Flash Attention
//    ai.llama.batch        n_batch（默认 512）
// ============================================================================
#include <llama.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zenith/abi/zenith_abi.h"
#include "zenith/ai/inference_service.hpp"

namespace {

const zenith_host_api* g_host = nullptr;
/// 默认静音 llama.cpp 的噪声日志（--verbose 或出错才输出到 stderr）
bool g_verbose = false;

void llamaLogBridge(ggml_log_level level, const char* text, void* /*user*/) {
    if (g_verbose || level >= GGML_LOG_LEVEL_ERROR) {
        std::fwrite(text, 1, std::strlen(text), stderr);
    }
}

void logAt(zenith_log_level level, const std::string& m) {
    if (g_host != nullptr && g_host->log_write != nullptr) {
        g_host->log_write(g_host->log_ctx, level, "ai_llama", 0, m.c_str());
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

std::string stemOf(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0) name.resize(dot);
    return name;
}

int64_t nowUnix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 把 token 转成 UTF-8 片段（缓冲不足时按返回的负数长度重来）
std::string pieceOf(const llama_vocab* vocab, llama_token id) {
    char buf[256];
    const int n = llama_token_to_piece(vocab, id, buf, static_cast<int32_t>(sizeof(buf)), 0, false);
    if (n >= 0) return std::string(buf, static_cast<size_t>(n));
    std::string big(static_cast<size_t>(-n), '\0');
    const int m = llama_token_to_piece(vocab, id, big.data(), static_cast<int32_t>(big.size()), 0,
                                       false);
    return m > 0 ? big.substr(0, static_cast<size_t>(m)) : std::string();
}

// ---------------------------------------------------------------------------
//  服务实现：一个模型 + 常驻上下文，请求串行（llama_context 非线程安全）
// ---------------------------------------------------------------------------
class LlamaService final : public zenith::ai::IInferenceService {
public:
    ~LlamaService() override { close(); }

    bool load(const std::string& path, const std::string& modelId, int nCtx, int gpuLayers,
              int threads, bool flashAttn, int nBatch) {
        llama_backend_init();
        llama_log_set(&llamaLogBridge, nullptr);

        llama_model_params mp = llama_model_default_params();
        mp.n_gpu_layers = gpuLayers;

        const auto t0 = std::chrono::steady_clock::now();
        model_ = llama_model_load_from_file(path.c_str(), mp);
        if (model_ == nullptr) return false;
        vocab_ = llama_model_get_vocab(model_);

        llama_context_params cp = llama_context_default_params();
        cp.n_ctx = static_cast<uint32_t>(nCtx > 0 ? nCtx : 4096);
        cp.n_batch = static_cast<uint32_t>(nBatch > 0 ? nBatch : 512);
        cp.n_threads = threads > 0 ? threads : std::max(1, static_cast<int>(
                                                              std::thread::hardware_concurrency() / 2));
        cp.n_threads_batch = cp.n_threads;
        cp.flash_attn_type =
            flashAttn ? LLAMA_FLASH_ATTN_TYPE_ENABLED : LLAMA_FLASH_ATTN_TYPE_AUTO;

        ctx_ = llama_init_from_model(model_, cp);
        if (ctx_ == nullptr) return false;
        nCtx_ = static_cast<int>(llama_n_ctx(ctx_));

        modelId_ = modelId.empty() ? stemOf(path) : modelId;
        char desc[256] = {0};
        if (llama_model_desc(model_, desc, sizeof(desc)) < 0) desc[0] = '\0';
        desc_ = desc;
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - t0)
                              .count();
        logInfo("ai_llama: model loaded in " + std::to_string(static_cast<int>(ms)) +
                " ms | n_ctx=" + std::to_string(nCtx_) + " | " + desc_);
        return true;
    }

    void close() {
        if (ctx_ != nullptr) {
            llama_free(ctx_);
            ctx_ = nullptr;
        }
        if (model_ != nullptr) {
            llama_model_free(model_);
            model_ = nullptr;
        }
        llama_backend_free();
    }

    [[nodiscard]] std::vector<zenith::ai::ModelInfo> models() override {
        zenith::ai::ModelInfo info;
        info.id = modelId_;
        info.ownedBy = "zenith";
        info.createdAt = nowUnix();
        info.contextSize = nCtx_;
        info.backend = "llama.cpp-inproc";
        return {info};
    }

    zenith::ai::ChatResult chat(const zenith::ai::ChatRequest& req,
                                const zenith::ai::TokenCallback& onToken,
                                const zenith::ai::TokenCallback& onReasoning) override {
        (void)onReasoning; // llama.cpp 不做 reasoning 分离（思考在模板里）
        zenith::ai::ChatResult r;
        r.model = req.model.empty() ? modelId_ : req.model;

        std::lock_guard<std::mutex> lk(mtx_);
        if (ctx_ == nullptr || vocab_ == nullptr) {
            r.error = "model not loaded";
            return r;
        }

        // ---- 1) 模板 → prompt ----
        std::vector<llama_chat_message> msgs;
        msgs.reserve(req.messages.size());
        size_t totalChars = 0;
        for (const auto& m : req.messages) {
            msgs.push_back(llama_chat_message{m.role.c_str(), m.content.c_str()});
            totalChars += m.role.size() + m.content.size();
        }
        const char* tmpl = llama_model_chat_template(model_, nullptr);
        std::string prompt(totalChars * 2 + 1024, '\0');
        int32_t n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, prompt.data(),
                                              static_cast<int32_t>(prompt.size()));
        if (n > static_cast<int32_t>(prompt.size())) {
            prompt.resize(static_cast<size_t>(n));
            n = llama_chat_apply_template(tmpl, msgs.data(), msgs.size(), true, prompt.data(),
                                          static_cast<int32_t>(prompt.size()));
        }
        if (n <= 0) {
            r.error = "chat template apply failed";
            return r;
        }
        prompt.resize(static_cast<size_t>(n));

        // ---- 2) 分词 ----
        std::vector<llama_token> tokens(static_cast<size_t>(prompt.size()) + 16);
        int32_t nt = llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()),
                                    tokens.data(), static_cast<int32_t>(tokens.size()), true, true);
        if (nt < 0) {
            tokens.resize(static_cast<size_t>(-nt));
            nt = llama_tokenize(vocab_, prompt.data(), static_cast<int32_t>(prompt.size()),
                                tokens.data(), static_cast<int32_t>(tokens.size()), true, true);
        }
        if (nt <= 0) {
            r.error = "tokenization failed";
            return r;
        }
        tokens.resize(static_cast<size_t>(nt));
        r.usage.promptTokens = nt;

        const int limit = req.maxTokens > 0 ? req.maxTokens : 512;
        if (static_cast<int>(tokens.size()) + limit >= nCtx_) {
            r.error = "prompt + max_tokens exceeds n_ctx (" + std::to_string(nCtx_) +
                      "); raise ai.llama.context";
            return r;
        }

        // ---- 3) 清空 KV 并预填 ----
        llama_memory_t mem = llama_get_memory(ctx_);
        if (mem != nullptr) llama_memory_clear(mem, true);

        const auto tPrompt0 = std::chrono::steady_clock::now();
        const int nBatch = 512;
        for (size_t off = 0; off < tokens.size(); off += static_cast<size_t>(nBatch)) {
            const size_t chunk = std::min<size_t>(static_cast<size_t>(nBatch), tokens.size() - off);
            llama_batch batch = llama_batch_get_one(tokens.data() + off,
                                                    static_cast<int32_t>(chunk));
            if (llama_decode(ctx_, batch) != 0) {
                r.error = "prompt decode failed";
                return r;
            }
        }
        const double promptMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tPrompt0)
                .count();

        // ---- 4) 采样循环 ----
        llama_sampler_chain_params sp = llama_sampler_chain_default_params();
        llama_sampler* smpl = llama_sampler_chain_init(sp);
        if (smpl == nullptr) {
            r.error = "sampler init failed";
            return r;
        }
        if (req.temperature <= 0.0) {
            llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
        } else {
            llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
            llama_sampler_chain_add(smpl, llama_sampler_init_top_p(static_cast<float>(req.topP), 1));
            llama_sampler_chain_add(smpl,
                                    llama_sampler_init_temp(static_cast<float>(req.temperature)));
            const uint32_t seed = req.seed >= 0 ? static_cast<uint32_t>(req.seed) : 0xC0FFEEu;
            llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed));
        }

        const auto tGen0 = std::chrono::steady_clock::now();
        bool aborted = false;
        int generated = 0;
        for (int i = 0; i < limit; ++i) {
            const llama_token id = llama_sampler_sample(smpl, ctx_, -1);
            llama_sampler_accept(smpl, id);
            if (llama_vocab_is_eog(vocab_, id)) {
                r.finishReason = "stop";
                break;
            }
            std::string piece = pieceOf(vocab_, id);
            if (piece.empty()) continue;
            r.content += piece;
            ++generated;

            if (onToken && !onToken(piece)) {
                aborted = true;
                r.finishReason = "aborted";
                break;
            }
            bool hitStop = false;
            for (const auto& s : req.stop) {
                if (!s.empty() && r.content.size() >= s.size() &&
                    r.content.compare(r.content.size() - s.size(), s.size(), s) == 0) {
                    hitStop = true;
                    break;
                }
            }
            if (hitStop) {
                r.finishReason = "stop";
                break;
            }
            llama_batch one = llama_batch_get_one(const_cast<llama_token*>(&id), 1);
            if (llama_decode(ctx_, one) != 0) {
                r.finishReason = "stop";
                break;
            }
            if (i == limit - 1) r.finishReason = "length";
        }
        const double genMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tGen0)
                .count();

        llama_sampler_free(smpl);

        r.usage.completionTokens = generated;
        r.stats.valid = true;
        r.stats.prefillTps = promptMs > 0 ? r.usage.promptTokens / (promptMs / 1000.0) : 0.0;
        r.stats.decodeTps = genMs > 0 ? generated / (genMs / 1000.0) : 0.0;
        r.ok = !aborted || !r.content.empty();
        if (!r.ok) r.error = "aborted before any token";
        return r;
    }

    zenith::ai::EmbeddingResult embeddings(const std::string& model, const std::string&) override {
        zenith::ai::EmbeddingResult r;
        r.model = model.empty() ? modelId_ : model;
        r.error = "llama.cpp in-process backend 未提供 embeddings（请用专用 embedding 模型或上游）";
        return r;
    }

    [[nodiscard]] std::string backend() const override { return "llama.cpp-inproc"; }

private:
    llama_model* model_ = nullptr;
    const llama_vocab* vocab_ = nullptr;
    llama_context* ctx_ = nullptr;
    int nCtx_ = 0;
    std::string modelId_;
    std::string desc_;
    std::mutex mtx_;
};

void destroyService(void* p) { delete static_cast<LlamaService*>(p); }

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    const std::string path = cfgStr("ai.llama.model", "");
    if (path.empty()) {
        logError("ai_llama: ai.llama.model is empty");
        return ZENITH_ERR_INVALID_ARGUMENT;
    }
    const std::string modelId = cfgStr("ai.llama.model_id", "");
    const int nCtx = cfgInt("ai.llama.context", 4096);
    const int gpuLayers = cfgInt("ai.llama.gpu_layers", 0);
    const int threads = cfgInt("ai.llama.threads", 0);
    const int nBatch = cfgInt("ai.llama.batch", 512);
    const bool flashAttn = cfgInt("ai.llama.flash_attn", 0) != 0;
    g_verbose = cfgInt("ai.llama.verbose", 0) != 0;

    auto* svc = new (std::nothrow) LlamaService();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    if (!svc->load(path, modelId, nCtx, gpuLayers, threads, flashAttn, nBatch)) {
        logError("ai_llama: failed to load model: " + path);
        delete svc;
        return ZENITH_ERR_INVALID_ARGUMENT;
    }

    // 注册独立服务名：允许与 ai_ninfer / ai_inference 同进程共存而不撞键
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  "zenith.ai.ILlamaInference", 1, static_cast<void*>(svc),
                                  &destroyService);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_llama",
    "0.1.0",
    "Zenith AI plugin: in-process llama.cpp via its pure C ABI (llama.h)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
