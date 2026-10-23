#include "zenith/ai/ninfer_c_api_client.hpp"

#include <chrono>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace zenith::ai {

namespace {

/// 回调上下文：C 函数指针 + user 指针，跨 DLL 只传这两个
struct CallbackCtx {
    const TokenCallback* onToken = nullptr;     ///< 正文
    const TokenCallback* onReasoning = nullptr; ///< 思考过程（ABI v2）
    bool aborted = false;
};

/// ABI v1 回调（只有正文）。v2 宿主仍支持它：思考内容退化为聚合结果。
int32_t NZ_CALL tokenTrampolineV1(const char* piece, int32_t len, void* user) {
    auto* ctx = static_cast<CallbackCtx*>(user);
    if (ctx == nullptr || len <= 0 || ctx->onToken == nullptr || !(*ctx->onToken)) return 0;
    if (!(*ctx->onToken)(std::string_view(piece, static_cast<size_t>(len)))) {
        ctx->aborted = true;
        return 1;
    }
    return 0;
}

/// ABI v2：按 channel 分流到正文/思考回调
int32_t NZ_CALL tokenTrampoline(const char* piece, int32_t len, int32_t channel, void* user) {
    auto* ctx = static_cast<CallbackCtx*>(user);
    if (ctx == nullptr || len <= 0) return 0;
    const bool isReasoning = channel == ZENITH_NZ_CHANNEL_REASONING;
    const TokenCallback* sink = isReasoning ? ctx->onReasoning : ctx->onToken;
    if (sink == nullptr || !(*sink)) return 0; // 这一路没人要，继续跑但丢弃
    if (!(*sink)(std::string_view(piece, static_cast<size_t>(len)))) {
        ctx->aborted = true;
        return 1; // 请求引擎中止
    }
    return 0;
}

class NinferCAbiService final : public IInferenceService {
public:
    NinferCAbiService(const zenith_nz_service* svc, std::string modelId, std::string artifactPath)
        : svc_(svc), modelId_(std::move(modelId)), artifact_(std::move(artifactPath)),
          abiVersion_(svc != nullptr ? svc->version : 0u) {}

    [[nodiscard]] std::vector<ModelInfo> models() override {
        ModelInfo info;
        info.id = modelId_;
        info.ownedBy = "zenith";
        info.createdAt = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count());
        info.contextSize = 0; // shim 未暴露实际 n_ctx；参看 artifact
        info.backend = "ninfer-c-abi";
        return {info};
    }

    ChatResult chat(const ChatRequest& req, const TokenCallback& onToken,
                    const TokenCallback& onReasoning) override {
        ChatResult r;
        r.model = req.model.empty() ? modelId_ : req.model;
        if (svc_ == nullptr || svc_->generate == nullptr || svc_->engine == nullptr) {
            r.error = "ninfer c-abi service not registered";
            return r;
        }

        // 先固化字符串，再让 C 结构指向它们（避免 vector 重分配悬垂）
        std::vector<std::string> roles;
        std::vector<std::string> contents;
        roles.reserve(req.messages.size());
        contents.reserve(req.messages.size());
        for (const auto& m : req.messages) {
            roles.push_back(m.role);
            contents.push_back(m.content);
        }
        std::vector<zenith_nz_message> msgs;
        msgs.reserve(req.messages.size());
        for (size_t i = 0; i < roles.size(); ++i) {
            zenith_nz_message m{};
            m.role = roles[i].c_str();
            m.content = contents[i].c_str();
            msgs.push_back(m);
        }

        zenith_nz_sampling sampling{};
        sampling.struct_size = sizeof(zenith_nz_sampling);
        sampling.temperature = static_cast<float>(req.temperature);
        sampling.top_p = static_cast<float>(req.topP);
        sampling.top_k = 0; // 用引擎默认
        sampling.seed = req.seed >= 0 ? static_cast<uint32_t>(req.seed) : 0;
        sampling.enable_thinking = req.thinking;

        zenith_nz_result out{};
        out.struct_size = sizeof(zenith_nz_result);
        CallbackCtx ctx;
        ctx.onToken = &onToken;
        ctx.onReasoning = &onReasoning; // ABI v2：思考过程实时回调

        // v1 插件的回调只接受 (piece,len,user)；把 3 参数函数传进 4 参数槽是安全的
        // —— Win64 下被调方忽略多余的 channel 实参。v2 则走带 channel 的版本。
        const zenith_nz_token_cb cb =
            abiVersion_ >= 2 ? &tokenTrampoline
                             : reinterpret_cast<zenith_nz_token_cb>(&tokenTrampolineV1);
        const int32_t rc =
            svc_->generate(svc_->engine, msgs.data(), static_cast<int32_t>(msgs.size()),
                           req.maxTokens, &sampling, cb, &ctx, &out);
        if (rc != 0 || out.status != 0) {
            r.error = out.error[0] != '\0' ? std::string(out.error)
                                           : (svc_->last_error != nullptr
                                                  ? std::string(svc_->last_error(svc_->engine))
                                                  : std::string("ninfer generate failed"));
            return r;
        }

        r.content.assign(out.content, out.content_len);
        r.reasoning = out.reasoning;
        r.usage.promptTokens = static_cast<int>(out.prompt_tokens);
        r.usage.completionTokens = static_cast<int>(out.completion_tokens);
        r.finishReason = finishReasonText(out.finish_reason);
        r.stats.valid = true;
        if (out.prefill_seconds > 0.0) {
            r.stats.prefillTps = static_cast<double>(out.prompt_tokens) / out.prefill_seconds;
        }
        if (out.decode_seconds > 0.0) {
            r.stats.decodeTps = static_cast<double>(out.completion_tokens) / out.decode_seconds;
        }
        if (out.first_token_seconds > 0.0) {
            r.stats.firstTokenMs = out.first_token_seconds * 1000.0;
        }
        r.stats.draftTotal = static_cast<int64_t>(out.drafted_tokens);
        r.stats.draftAccepted = static_cast<int64_t>(out.accepted_tokens);
        lastContextTokens_ = static_cast<int64_t>(r.usage.promptTokens) + r.usage.completionTokens;
        r.ok = !ctx.aborted || !r.content.empty();
        if (ctx.aborted) r.finishReason = "aborted";
        return r;
    }

    EmbeddingResult embeddings(const std::string& model, const std::string&) override {
        EmbeddingResult r;
        r.model = model.empty() ? modelId_ : model;
        r.error = "NInfer c-abi: embeddings 未暴露（引擎负责生成，不做 embedding 池化）";
        return r;
    }

    [[nodiscard]] std::string backend() const override { return "ninfer-c-abi (in-process)"; }

    [[nodiscard]] EngineState engineState() const override {
        EngineState st;
        if (svc_ == nullptr || svc_->stats == nullptr || svc_->engine == nullptr) return st;
        uint64_t reqs = 0;
        uint64_t drafted = 0;
        uint64_t accepted = 0;
        if (svc_->stats(svc_->engine, &reqs, nullptr, nullptr, &accepted, &drafted) == 0) {
            st.supervised = false; // 不是子进程，是进程内引擎
            st.running = true;
            st.requests = static_cast<int64_t>(reqs);
            st.draftAccepted = static_cast<int64_t>(accepted);
            st.draftTotal = static_cast<int64_t>(drafted);
            st.lastLine = artifact_.empty() ? std::string("in-process engine")
                                            : ("in-process: " + artifact_);
        }
        // 最近一次请求占用的上下文（v1/v2 插件只有这个数）
        st.lastContextTokens = lastContextTokens_;
        // ABI v3 起能读到 KV 真实占用
        if (abiVersion_ >= 3 && svc_->runtime != nullptr && svc_->engine != nullptr) {
            zenith_nz_runtime rt{};
            rt.struct_size = sizeof(zenith_nz_runtime);
            if (svc_->runtime(svc_->engine, &rt) == 0) {
                st.kvUsedTokens = static_cast<int64_t>(rt.kv_used_tokens);
                st.kvCapacityTokens = static_cast<int64_t>(rt.kv_capacity_tokens);
                st.kvHostUsedBytes = static_cast<int64_t>(rt.host_kv_used_bytes);
                st.kvHostCapacityBytes = static_cast<int64_t>(rt.host_kv_capacity_bytes);
                st.reusedTokens = static_cast<int64_t>(rt.reused_tokens);
            }
        }
        return st;
    }

private:
    static std::string finishReasonText(int32_t code) {
        switch (code) {
            case 1: return "length";
            case 2: return "context_capacity";
            case 3: return "stop";
            case 4: return "stop";
            case 5: return "cancelled";
            default: return "stop";
        }
    }

    const zenith_nz_service* svc_;
    std::uint32_t abiVersion_ = 0;
    std::int64_t lastContextTokens_ = 0;
    std::string modelId_;
    std::string artifact_;
};

} // namespace

std::unique_ptr<IInferenceService> wrapNinferCAbiService(const void* serviceTable,
                                                         std::string modelId,
                                                         std::string artifactPath) {
    if (serviceTable == nullptr) return nullptr;
    const auto* svc = static_cast<const zenith_nz_service*>(serviceTable);
    // 兼容 v1（只有正文回调，思考内容走聚合结果）与 v2（思考也实时流式）
    if (svc->version < 1 || svc->version > ZENITH_NINFER_C_API_VERSION) {
        std::fprintf(stderr,
                     "[ninfer-c-abi] 不支持的 ABI 版本：插件 v%u，宿主支持 v1..v%u\n",
                     svc->version, ZENITH_NINFER_C_API_VERSION);
        return nullptr;
    }
    if (svc->engine == nullptr ||
        svc->generate == nullptr) {
        return nullptr;
    }
    if (modelId.empty()) modelId = "ninfer-native";
    return std::make_unique<NinferCAbiService>(svc, std::move(modelId), std::move(artifactPath));
}

} // namespace zenith::ai
