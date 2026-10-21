// ============================================================================
//  ninfer_c_shim.cpp —— 把 NInfer 的 C++ API 包成纯 C（MSVC 编译，CUDA 引擎侧）
//
//  这是「像 llama.cpp 一样链接」的唯一可行做法：C++ 对象（ninfer::Engine、
//  std::string/std::vector/unique_ptr）绝不跨 DLL 边界，边界上只有 opaque 指针、
//  POD 结构、C 回调、调用方缓冲。
//
//  本文件由 MSVC 编译（CUDA 引擎只能 MSVC 编），与 MinGW 宿主之间只走
//  zenith/ai/ninfer_c_api.h 里的纯 C 契约。
// ============================================================================
#define ZENITH_NINFER_SHIM_BUILD 1
#include "zenith/ai/ninfer_c_api.h"

#include <ninfer/engine.h>
#include <ninfer/types.h>

#include <cstring>
#include <exception>
#include <memory>
#include <string>
#include <vector>

struct zenith_nz_engine {
    std::unique_ptr<ninfer::Engine> engine;
    std::string lastError;
    uint64_t requests = 0;
    uint64_t promptTokens = 0;
    uint64_t completionTokens = 0;
    uint64_t accepted = 0;
    uint64_t drafted = 0;
};

namespace {

ninfer::ChatRole roleFromString(const char* role) {
    if (role == nullptr) return ninfer::ChatRole::User;
    const std::string r(role);
    if (r == "system") return ninfer::ChatRole::System;
    if (r == "developer") return ninfer::ChatRole::Developer;
    if (r == "assistant") return ninfer::ChatRole::Assistant;
    if (r == "tool") return ninfer::ChatRole::Tool;
    return ninfer::ChatRole::User;
}

ninfer::KvCacheStorage kvFromCode(uint32_t code) {
    using S = ninfer::KvCacheStorage;
    switch (code) {
        case ZENITH_NZ_KV_INT8: return S::Int8Group64;
        case ZENITH_NZ_KV_FP8: return S::Fp8E4M3Row256;
        case ZENITH_NZ_KV_RK8V4: return S::RotatedInt8KeyInt4ValueGroup64;
        case ZENITH_NZ_KV_NVFP4: return S::Nvfp4Group16;
        case ZENITH_NZ_KV_K8V4: return S::Fp8KeyNvfp4Value;
        case ZENITH_NZ_KV_RK4V4: return S::RotatedLloyd4KeyInt4Value;
        case ZENITH_NZ_KV_RK4V4_E8: return S::RotatedInt4KeyInt4ValueE8;
        case ZENITH_NZ_KV_RK2V4_E8: return S::RotatedE8RootKeyInt4Value;
        case ZENITH_NZ_KV_BF16:
        default: return S::BFloat16;
    }
}

ninfer::SpeculativeBackend specFromCode(uint32_t code) {
    using B = ninfer::SpeculativeBackend;
    switch (code) {
        case ZENITH_NZ_SPEC_MTP: return B::Mtp;
        case ZENITH_NZ_SPEC_DFLASH: return B::DFlash;
        case ZENITH_NZ_SPEC_DFLASH2: return B::DFlash2;
        case ZENITH_NZ_SPEC_NONE:
        default: return B::None;
    }
}

void copyInto(char* dst, size_t cap, const std::string& src, bool* truncated) {
    if (cap == 0) return;
    const size_t n = src.size() < (cap - 1) ? src.size() : (cap - 1);
    std::memcpy(dst, src.data(), n);
    dst[n] = '\0';
    if (truncated != nullptr && n < src.size()) *truncated = true;
}

void setError(zenith_nz_engine* e, zenith_nz_result* out, const std::string& msg) {
    if (e != nullptr) e->lastError = msg;
    if (out != nullptr) {
        out->status = -1;
        copyInto(out->error, sizeof(out->error), msg, nullptr);
    }
}

/// 流式转发：把 NInfer 的 OutputSink 桥到 C 回调，同时累计正文
class BridgeSink final : public ninfer::OutputSink {
public:
    BridgeSink(zenith_nz_token_cb cb, void* user) : cb_(cb), user_(user) {}

    void start(ninfer::GenerationStart) override {}
    void progress(ninfer::PromptProgress) override {}
    void timing(ninfer::GenerationTimingObservation) override {}

    void publish(ninfer::OutputDelta delta) override {
        if (delta.channel == ninfer::OutputChannel::Content) {
            content += delta.text;
            if (cb_ != nullptr && !delta.text.empty()) {
                if (cb_(delta.text.data(), static_cast<int32_t>(delta.text.size()), user_) != 0) {
                    aborted = true;
                }
            }
        } else {
            reasoning += delta.text;
        }
    }

    std::string content;
    std::string reasoning;
    bool aborted = false;

private:
    zenith_nz_token_cb cb_;
    void* user_;
};

} // namespace

namespace {
/// 创建失败时的错误文本（进程级静态，插件 init 里读出来写日志）
char g_createError[1024] = {0};

void setCreateError(const std::string& msg) {
    const size_t n = msg.size() < sizeof(g_createError) - 1 ? msg.size() : sizeof(g_createError) - 1;
    std::memcpy(g_createError, msg.data(), n);
    g_createError[n] = '\0';
}
} // namespace

extern "C" {

NZ_EXPORT const char* NZ_CALL zenith_nz_last_create_error(void) { return g_createError; }

NZ_EXPORT zenith_nz_engine* NZ_CALL zenith_nz_create(const char* artifact_path,
                                                    const zenith_nz_config* cfg) {
    try {
        g_createError[0] = '\0';
        auto holder = std::make_unique<zenith_nz_engine>();
        ninfer::EngineOptions opt;
        opt.artifact_path = std::filesystem::path(artifact_path);
        opt.device = cfg != nullptr ? cfg->device : 0;

        // NInfer 的约束：kv_capacity >= max_context，且页数落在可用区间内。
        // 默认让两者相等（8192），调用方需要更长上下文时同时抬高这两个值。
        const uint32_t maxCtx =
            (cfg != nullptr && cfg->max_context != 0) ? cfg->max_context : 8192;
        const uint32_t kvTokens = (cfg != nullptr && cfg->kv_capacity_tokens != 0)
                                      ? cfg->kv_capacity_tokens
                                      : maxCtx;
        opt.max_context = maxCtx;
        opt.kv_capacity = ninfer::KvCapacityPolicy::explicit_capacity(kvTokens);
        if (cfg != nullptr && cfg->prefill_chunk != 0) opt.prefill_chunk = cfg->prefill_chunk;
        if (cfg != nullptr) {
            opt.kv_cache = kvFromCode(cfg->kv_dtype);
            opt.speculative.backend = specFromCode(cfg->spec_backend);
            opt.speculative.draft_tokens = cfg->draft_tokens;
            if (cfg->max_concurrency != 0) opt.max_concurrency = cfg->max_concurrency;
            opt.gdn_state_fp16 = cfg->gdn_state_fp16 != 0;
        }
        holder->engine = std::make_unique<ninfer::Engine>(std::move(opt));
        if (!holder->engine->is_available()) {
            setCreateError("engine reports unavailable after load (check --kv-capacity vs VRAM)");
            return nullptr;
        }
        return holder.release();
    } catch (const std::exception& ex) {
        setCreateError(std::string("engine create threw: ") + ex.what());
        return nullptr;
    } catch (...) {
        setCreateError("engine create threw unknown exception");
        return nullptr;
    }
}

NZ_EXPORT void NZ_CALL zenith_nz_destroy(zenith_nz_engine* e) { delete e; }

NZ_EXPORT const char* NZ_CALL zenith_nz_last_error(zenith_nz_engine* e) {
    return e != nullptr ? e->lastError.c_str() : "null engine";
}

NZ_EXPORT int32_t NZ_CALL zenith_nz_generate(zenith_nz_engine* e,
                                             const zenith_nz_message* messages, int32_t count,
                                             int32_t max_tokens,
                                             const zenith_nz_sampling* sampling,
                                             zenith_nz_token_cb cb, void* user,
                                             zenith_nz_result* out) {
    if (e == nullptr || e->engine == nullptr) {
        setError(e, out, "engine not created");
        return -1;
    }
    if (messages == nullptr || count <= 0) {
        setError(e, out, "no messages");
        return -2;
    }
    if (out != nullptr) out->struct_size = sizeof(zenith_nz_result);
    try {
        ninfer::PromptInput input;
        input.messages.reserve(static_cast<size_t>(count));
        for (int32_t i = 0; i < count; ++i) {
            ninfer::ChatMessage m;
            m.role = roleFromString(messages[i].role);
            ninfer::MessagePart part;
            part.kind = ninfer::MessagePartKind::Text;
            part.text = messages[i].content != nullptr ? messages[i].content : "";
            m.parts.push_back(std::move(part));
            input.messages.push_back(std::move(m));
        }

        ninfer::RequestOptions req;
        req.execution.requested_output_tokens =
            max_tokens > 0 ? static_cast<uint32_t>(max_tokens) : 256;
        if (sampling != nullptr) {
            if (sampling->temperature >= 0.0f) req.execution.sampling.temperature = sampling->temperature;
            if (sampling->top_p > 0.0f) req.execution.sampling.top_p = sampling->top_p;
            if (sampling->top_k > 0) req.execution.sampling.top_k = sampling->top_k;
            req.execution.sampling.seed = static_cast<uint64_t>(sampling->seed);
            // 思考开关在 PromptOptions（不是 RequestOptions）上
            if (sampling->enable_thinking != 0) {
                input.options.enable_thinking = sampling->enable_thinking > 0;
            }
        }

        auto prepared = e->engine->prepare(std::move(input));
        if (!prepared) {
            setError(e, out, "prompt preparation failed (context capacity?)");
            return -3;
        }

        BridgeSink sink(cb, user);
        const ninfer::GenerationResult res =
            e->engine->generate(std::move(prepared), req, cb != nullptr ? &sink : nullptr);

        // 正文/思考：sink 与引擎结果各取更长的一份。
        // 只取 sink 会在"思考很长、流式只回吐一部分"时把文本丢成 2 KB。
        const std::string& body = (cb != nullptr && sink.content.size() >= res.content.size())
                                      ? sink.content
                                      : res.content;
        const std::string& think = (cb != nullptr && sink.reasoning.size() >= res.reasoning.size())
                                       ? sink.reasoning
                                       : res.reasoning;

        ++e->requests;
        e->promptTokens += res.prompt.prompt_tokens;
        e->completionTokens += static_cast<uint64_t>(res.generated_token_ids.size());
        e->accepted += res.speculative.accepted_tokens;
        e->drafted += res.speculative.drafted_tokens;

        if (out != nullptr) {
            out->status = 0;
            out->finish_reason = static_cast<int32_t>(res.finish_reason);
            out->prompt_tokens = res.prompt.prompt_tokens;
            out->completion_tokens = static_cast<uint32_t>(res.generated_token_ids.size());
            out->reasoning_tokens = res.reasoning_tokens;
            out->reused_prompt_tokens = res.reused_prompt_tokens;
            out->first_token_seconds = res.timings.first_token_seconds;
            out->prefill_seconds = res.timings.prefill_seconds;
            out->decode_seconds = res.timings.decode_seconds;
            out->total_seconds = res.timings.total_seconds;
            out->drafted_tokens = res.speculative.drafted_tokens;
            out->accepted_tokens = res.speculative.accepted_tokens;
            out->content_truncated = 0;
            bool truncated = false;
            copyInto(out->content, sizeof(out->content), body, &truncated);
            copyInto(out->reasoning, sizeof(out->reasoning), think, nullptr);
            out->content_len = static_cast<uint32_t>(std::strlen(out->content));
            out->content_truncated = truncated ? 1u : 0u;
            out->error[0] = '\0';
        }
        return 0;
    } catch (const std::exception& ex) {
        setError(e, out, ex.what());
        return -4;
    } catch (...) {
        setError(e, out, "unknown native exception");
        return -5;
    }
}

NZ_EXPORT int32_t NZ_CALL zenith_nz_stats(zenith_nz_engine* e, uint64_t* requests,
                                          uint64_t* prompt_tokens, uint64_t* completion_tokens,
                                          uint64_t* accepted, uint64_t* drafted) {
    if (e == nullptr) return -1;
    if (requests != nullptr) *requests = e->requests;
    if (prompt_tokens != nullptr) *prompt_tokens = e->promptTokens;
    if (completion_tokens != nullptr) *completion_tokens = e->completionTokens;
    if (accepted != nullptr) *accepted = e->accepted;
    if (drafted != nullptr) *drafted = e->drafted;
    return 0;
}

} // extern "C"
