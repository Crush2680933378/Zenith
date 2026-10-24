// ============================================================================
//  zenith::ai::ProviderRouter —— 多厂商 LLM API 路由（M12）
//
//  参考 DeepSeek Harness 的适配器模式：每个厂商一条「协议 + baseURL + key +
//  模型清单」配置；路由按模型名选厂商，协议适配器负责双向翻译：
//
//    协议          wire 格式                          厂商举例
//    openai-chat   POST /chat/completions (Bearer)    DeepSeek/OpenAI/Kimi/GLM/
//                                                     SiliconFlow/任何兼容网关
//    anthropic     POST /v1/messages (x-api-key)      Anthropic Claude
//    gemini        POST /models/{m}:generateContent   Google Gemini
//
//  模型发现：openai 协议走 GET {baseURL}/models（data[].id）。
//  流式：内部 chat 走非流式（stream:false）整包翻译；对外 SSE 由
//  openai_api 网关层承担（整段一次推出）。逐 token 上游流式列 M12 后续。
//
//  配置（JSON，同 DeepSeek Harness providers 语义）：
//  {
//    "providers": [
//      {"name":"deepseek", "api":"openai-chat",
//       "baseURL":"https://api.deepseek.com/v1", "apiKey":"sk-..",
//       "models":["deepseek-chat","deepseek-reasoner"]},
//      {"name":"anthropic", "api":"anthropic",
//       "baseURL":"https://api.anthropic.com", "apiKey":"sk-ant-..",
//       "models":["claude-sonnet-4-5"]},
//      {"name":"gemini", "api":"gemini",
//       "baseURL":"https://generativelanguage.googleapis.com/v1beta",
//       "apiKey":"AIza..", "models":["gemini-2.5-pro"]}
//    ],
//    "routes": {"claude-*":"anthropic", "gemini-*":"gemini"},   // 可选通配
//    "defaultProvider": "deepseek"                               // 可选兜底
//  }
//  apiKey 支持环境变量引用："$ENV:DEEPSEEK_API_KEY"。
// ============================================================================
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "zenith/ai/inference_service.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"

namespace zenith::ai {

enum class ProviderApi : uint8_t {
    OpenAiChat = 0,   // OpenAI Chat Completions（DeepSeek 等）
    Anthropic,        // Anthropic Messages
    Gemini,           // Google generateContent
};

ZENITH_API const char* providerApiName(ProviderApi api) noexcept;
ZENITH_API core::Result<ProviderApi> providerApiFrom(const std::string& s);

struct ProviderConfig {
    std::string name;
    ProviderApi api = ProviderApi::OpenAiChat;
    std::string baseURL;         // 含到版本段（openai: .../v1；anthropic: ...根；gemini: .../v1beta）
    std::string apiKey;          // 支持 "$ENV:VAR" 引用
    std::vector<std::string> models;
    int timeoutMs = 120000;
};

struct RouterConfig {
    std::vector<ProviderConfig> providers;
    std::map<std::string, std::string> routes; // 通配 "claude-*" → 厂商名
    std::string defaultProvider;               // 空 = 无兜底（未知模型报错）

    /// 解析配置 JSON（失败容忍：单条 provider 非法跳过）
    static core::Result<RouterConfig> fromJson(const json::JsonValue& v);
};

/// 多厂商路由推理服务——实现 IInferenceService（ai_crew/ai_agent/openai_api
/// 网关零改动获得多厂商能力）。线程安全：配置装载后只读。
class ZENITH_API ProviderRouter final : public IInferenceService {
public:
    explicit ProviderRouter(RouterConfig cfg);

    // ---- IInferenceService ----
    std::vector<ModelInfo> models() override;
    ChatResult chat(const ChatRequest& req, const TokenCallback& onToken = nullptr,
                    const TokenCallback& onReasoning = nullptr) override;
    EmbeddingResult embeddings(const std::string& model, const std::string& input) override;
    std::string backend() const override { return "provider-router"; }
    [[nodiscard]] EngineState engineState() const override;

    // ---- 路由 ----
    /// 模型 → 厂商；无匹配返回空
    [[nodiscard]] const ProviderConfig* route(const std::string& model) const;
    /// 模型发现（openai 协议 GET /models；其余返回配置内清单）
    core::Result<std::vector<std::string>> discoverModels(const std::string& providerName);
    /// 上次路由命中的厂商（诊断；thread_local 每请求有效）
    static thread_local std::string lastProviderUsed;

    [[nodiscard]] const RouterConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] size_t providerCount() const noexcept { return cfg_.providers.size(); }

private:
    ChatResult chatOpenAi(const ProviderConfig& p, const ChatRequest& req);
    ChatResult chatAnthropic(const ProviderConfig& p, const ChatRequest& req);
    ChatResult chatGemini(const ProviderConfig& p, const ChatRequest& req);

    RouterConfig cfg_;
    std::map<std::string, size_t> byName_; // 厂商名 → 下标
};

/// 一次极简阻塞 HTTP（POST/GET，Content-Length/chunked/读到关闭；http:// 明文）。
/// 与 ai_inference 插件内 upstream_http 同构需求的最小实现——独立于插件，
/// 供 ProviderRouter 调用厂商端点（https 上游经前置反代或本机代理转 http）。
struct ZENITH_API SimpleHttpCall {
    std::string method = "POST";
    std::string url; // http://host[:port]/path[?query]
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 120000;
};
struct ZENITH_API SimpleHttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};
ZENITH_API SimpleHttpResult simpleHttpRequest(const SimpleHttpCall& call);

} // namespace zenith::ai
