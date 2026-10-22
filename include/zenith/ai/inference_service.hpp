// ============================================================================
//  zenith::ai::IInferenceService —— LLM 推理服务契约（M8.1）
//  宿主与 ai_inference 插件之间的共享接口；第三方可依同契约写别的后端
//  （llama.cpp / vLLM / 远端 API），OpenAI 兼容层只认这个接口。
//
//  边界规则：C++ 虚接口仅在「宿主编译器与运行库一致」时穿越动态库；
//  跨编译器场景请退回 abi/zenith_abi.h 的纯 C 函数表。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/plugin/service.hpp"

namespace zenith::ai {

/// 一条对话消息（OpenAI 语义：role ∈ system/user/assistant/tool）
struct ChatMessage {
    std::string role = "user";
    std::string content;
    std::string name; // 可选（多角色/工具场景）
};

/// 推理请求（OpenAI 参数子集 + 后端透传位）
struct ChatRequest {
    std::string model;              // 空 = 服务端默认模型
    std::vector<ChatMessage> messages;
    double temperature = 0.7;
    double topP = 1.0;
    int maxTokens = 512;
    std::vector<std::string> stop;  // 命中任一即停止
    int seed = -1;                  // <0 = 不固定
    bool stream = false;            // 仅表达调用方意图，实际由宿主选通道
    int thinking = 0;               // -1 关闭思考 / 0 后端默认 / 1 强制开启
};

struct Usage {
    int promptTokens = 0;
    int completionTokens = 0;
    [[nodiscard]] int totalTokens() const noexcept { return promptTokens + completionTokens; }
};

/// 单次请求的引擎侧测速读数（llama.cpp / NInfer 的 `timings` 字段）
struct EngineStats {
    bool valid = false;
    double prefillTps = 0;     // prompt_per_second
    double decodeTps = 0;      // predicted_per_second
    int64_t draftTotal = 0;    // draft_n（投机解码候选数）
    int64_t draftAccepted = 0; // draft_n_accepted
    int64_t cachedTokens = 0;  // cache_n / prompt cache 命中
    double firstTokenMs = 0;   // 首 token 延迟（进程内后端才有）
};

/// 引擎进程运行态（仅「本地引擎」形态有内容）
struct EngineState {
    bool supervised = false; // 是否由本插件拉起并监管
    bool running = false;
    int64_t pid = 0;
    int restarts = 0;
    int64_t requests = 0; // 引擎控制台累计请求数
    double lastTtftMs = 0;
    double lastPrefillTps = 0;
    double lastDecodeTps = 0;
    int64_t draftAccepted = 0;
    int64_t draftTotal = 0;
    std::string lastLine; // 引擎最近一行日志（诊断用）
};

struct ChatResult {
    bool ok = false;
    std::string id;           // chatcmpl-xxx
    std::string model;
    std::string content;      // 完整回复（流式时同样回填，便于审计/日志）
    std::string reasoning;    // 思考模型的推理过程（reasoning_content）
    Usage usage;
    std::string finishReason = "stop"; // stop / length / aborted
    std::string error;
    EngineStats stats;        // 引擎测速读数（后端不支持时 valid=false）
};

struct EmbeddingResult {
    bool ok = false;
    std::vector<float> vector;
    std::string model;
    Usage usage;
    std::string error;
};

struct ModelInfo {
    std::string id;
    std::string ownedBy = "zenith";
    int64_t createdAt = 0;
    int contextSize = 0;   // 0 = 未知
    std::string backend;   // ninfer / llama.cpp / mock
};

/// 逐 token 回调：返回 false 表示调用方要求提前中止（客户端断开等）
using TokenCallback = std::function<bool(std::string_view delta)>;

class IInferenceService : public zenith::plugin::IService {
public:
    ZENITH_SERVICE_DECL(IInferenceService, "zenith.ai.IInferenceService", 1)

    /// 已加载模型列表（线程安全；可随时调用）
    [[nodiscard]] virtual std::vector<ModelInfo> models() = 0;

    /// 对话补全（阻塞调用方线程——宿主须经线程池调用，绝不占 EventLoop）。
    /// onToken 非空时逐片回调正式回复；onReasoning 非空时逐片回调推理过程
    /// （思考模型的 reasoning_content，可只收集不推给终端用户）。
    /// 任一回调返回 false 即中止，finish_reason 置 aborted。
    virtual ChatResult chat(const ChatRequest& req, const TokenCallback& onToken = nullptr,
                            const TokenCallback& onReasoning = nullptr) = 0;

    /// 文本嵌入（阻塞，同上）
    virtual EmbeddingResult embeddings(const std::string& model, const std::string& input) = 0;

    /// 后端标识（"ninfer" / "openai-upstream" / "mock"），健康检查与 status 页用
    [[nodiscard]] virtual std::string backend() const = 0;

    /// 引擎进程运行态；非引擎形态返回默认值（supervised=false）
    [[nodiscard]] virtual EngineState engineState() const { return EngineState{}; }
};

} // namespace zenith::ai
