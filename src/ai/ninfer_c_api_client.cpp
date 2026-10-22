#include "zenith/ai/ninfer_c_api_client.hpp"

#include <chrono>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace zenith::ai {

namespace {

/// 回调上下文：C 函数指针 + user 指针，跨 DLL 只传这两个
struct CallbackCtx {
    const TokenCallback* onToken = nullptr;
    bool aborted = false;
};

int32_t NZ_CALL tokenTrampoline(const char* piece, int32_t len, void* user) {
    auto* ctx = static_cast<CallbackCtx*>(user);
    if (ctx == nullptr || ctx->onToken == nullptr || !(*ctx->onToken)) return 0;
    if (len <= 0) return 0;
    if (!(*ctx->onToken)(std::string_view(piece, static_cast<size_t>(len)))) {
        ctx->aborted = true;
        return 1; // 请求引擎中止
    }
    return 0;
}

class NinferCAbiService final : public IInferenceService {
public:
    NinferCAbiService(const zenith_nz_service* svc, std::string modelId, std::string artifactPath)
        : svc_(svc), modelId_(std::move(modelId)), artifact_(std::move(artifactPath)) {}

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
        (void)onReasoning; // shim 目前只回传正文回调；推理片段走 reasoning 字段
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

        const int32_t rc =
            svc_->generate(svc_->engine, msgs.data(), static_cast<int32_t>(msgs.size()),
                           req.maxTokens, &sampling, &tokenTrampoline, &ctx, &out);
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
    std::string modelId_;
    std::string artifact_;
};

} // namespace

std::unique_ptr<IInferenceService> wrapNinferCAbiService(const void* serviceTable,
                                                         std::string modelId,
                                                         std::string artifactPath) {
    if (serviceTable == nullptr) return nullptr;
    const auto* svc = static_cast<const zenith_nz_service*>(serviceTable);
    if (svc->version != ZENITH_NINFER_C_API_VERSION || svc->engine == nullptr ||
        svc->generate == nullptr) {
        return nullptr;
    }
    if (modelId.empty()) modelId = "ninfer-native";
    return std::make_unique<NinferCAbiService>(svc, std::move(modelId), std::move(artifactPath));
}

} // namespace zenith::ai
