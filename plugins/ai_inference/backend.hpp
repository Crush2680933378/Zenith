// ============================================================================
//  ai_inference 插件内部后端抽象
//  - MockBackend        ：确定性后端（无模型也能验证 OpenAI 契约全链路、跑 CI）
//  - OpenAiUpstreamBackend：任何 OpenAI 兼容服务（本地引擎走这里，由
//                           EngineSupervisor 负责进程生命周期）
//  两者同接口，宿主/适配层只认 IInferenceService。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zenith/ai/inference_service.hpp"

namespace zenith::ai::detail {

class Backend {
public:
    virtual ~Backend() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual std::vector<ModelInfo> models() const = 0;
    virtual ChatResult chat(const ChatRequest& req, const TokenCallback& onToken,
                            const TokenCallback& onReasoning) = 0;
    virtual EmbeddingResult embeddings(const std::string& model, const std::string& input) = 0;
};

/// 确定性 mock 后端：回复 = "mock: <最后一条 user 内容>"，按固定宽度切片流式回放
std::unique_ptr<Backend> makeMockBackend(std::string modelId, int embeddingDim);

/// OpenAI 兼容上游后端：把「任何 OpenAI 兼容服务」当推理后端。
/// 本地引擎（NInfer / llama-server）与云端 API 同走这条路径；
/// 同时解析响应里的 `timings`，把投机解码/预填解码速率带回 ChatResult.stats。
/// baseUrl 形如 http://127.0.0.1:8091；apiKey 为空则不带 Authorization。
std::unique_ptr<Backend> makeOpenAiUpstreamBackend(std::string baseUrl, std::string apiKey,
                                                   std::string model, int timeoutMs);

} // namespace zenith::ai::detail
