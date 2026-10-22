// ============================================================================
//  zenith::ai::mountOpenAiApi —— OpenAI 兼容 API 适配层（M8.1）
//  把 zen::ai::IInferenceService 暴露成 /v1/models、/v1/chat/completions、
//  /v1/completions、/v1/embeddings（含 SSE 流式），现有 OpenAI 客户端零改动接入。
// ============================================================================
#pragma once

#include <string>

#include "zenith/ai/inference_service.hpp"
#include "zenith/core/export.hpp"
#include "zenith/http/router.hpp"

namespace zenith::concurrency {
class ThreadPool;
}

namespace zenith::ai {

struct OpenAiApiOptions {
    std::string prefix = "/v1";
    /// 非空：推理任务投递到该池执行（推荐）——绝不占 EventLoop 线程。
    /// 为空：就地同步执行（测试/单机 CLI 形态）。
    concurrency::ThreadPool* pool = nullptr;
    /// 流式末片是否附带 usage（OpenAI 的 stream_options.include_usage 语义）
    bool includeUsageInStream = true;
    /// 是否打点 Prometheus 指标（zenith_ai_*）
    bool metricsEnabled = true;
    /// 后端默认的思考模式（-1 关 / 0 用后端默认 / 1 开）。
    /// 请求体里的 `thinking` 字段若存在则覆盖它——用于"启动参数定默认、单次请求可改"。
    int defaultThinking = 0;
};

/// 挂载 OpenAI 兼容路由。service 生命周期须覆盖 router 的调用期。
ZENITH_API void mountOpenAiApi(http::Router& router, IInferenceService& service,
                               const OpenAiApiOptions& options = {});

} // namespace zenith::ai
