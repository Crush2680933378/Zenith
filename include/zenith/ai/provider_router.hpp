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
//  流式（M13.3）：chat() 传入 onToken 时走上游真流式（stream:true /
//  streamGenerateContent?alt=sse），三种协议的 SSE 事件在内部翻译成统一
//  token 增量回调（openai delta.content / anthropic text_delta / gemini
//  parts[].text；reasoning 增量同理）。onToken 为空保持整包非流式路径。
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
//  M13 第二批可选字段（provider 级）：
//    "fallbackTo":"backup"                    失败转移目标厂商
//    "circuit":{"failThreshold":3,"openMs":30000}   熔断（failThreshold=0 关闭）
//    "pricing":{"promptPerM":0.27,"completionPerM":1.10}  成本记账（$/百万 token）
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
    bool insecure = false;       // 跳过证书校验（自签/测试网关）
    std::string fallbackTo;      // 失败转移目标厂商名（M13 第二批）
    // ---- 熔断（M13 第二批；failThreshold=0 关闭）----
    int cbFailThreshold = 3;     // 连续失败 N 次后熔断
    int cbOpenMs = 30000;        // 熔断冷却（半开试探前）
    // ---- 成本记账（美元 / 每百万 token；0 = 不计价）----
    double pricePromptPerM = 0.0;
    double priceCompletionPerM = 0.0;
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
    ~ProviderRouter() override; // rt_（pimpl）在 .cpp 完成析构

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

    // ---- 可观测（M13 第二批：failover/熔断/成本）----
    struct ProviderStat {
        uint64_t calls = 0;        // 实际打到该厂商的请求数
        uint64_t failures = 0;
        uint64_t fallbacks = 0;    // 因该厂商失败而转移走的次数
        uint64_t tokensIn = 0;
        uint64_t tokensOut = 0;
        double costUsd = 0.0;
        const char* cbState = "closed"; // closed / open / half-open
    };
    /// 各厂商运行统计快照（含熔断状态）
    [[nodiscard]] std::map<std::string, ProviderStat> statsSnapshot() const;

    [[nodiscard]] const RouterConfig& config() const noexcept { return cfg_; }
    [[nodiscard]] size_t providerCount() const noexcept { return cfg_.providers.size(); }

private:
    ChatResult chatOpenAi(const ProviderConfig& p, const ChatRequest& req,
                          const TokenCallback& onToken, const TokenCallback& onReasoning);
    ChatResult chatAnthropic(const ProviderConfig& p, const ChatRequest& req,
                             const TokenCallback& onToken, const TokenCallback& onReasoning);
    ChatResult chatGemini(const ProviderConfig& p, const ChatRequest& req,
                          const TokenCallback& onToken, const TokenCallback& onReasoning);

    RouterConfig cfg_;
    std::map<std::string, size_t> byName_; // 厂商名 → 下标
    struct Rt;                             // 运行态（熔断/统计，pimpl 隐藏锁与时钟）
    std::unique_ptr<Rt> rt_;
};

/// 一次极简阻塞 HTTP（POST/GET，Content-Length/chunked/读到关闭；http:// 明文）。
/// 与 ai_inference 插件内 upstream_http 同构需求的最小实现——独立于插件，
/// 供 ProviderRouter 调用厂商端点（https 上游经前置反代或本机代理转 http）。
struct ZENITH_API SimpleHttpCall {
    std::string method = "POST";
    std::string url; // http:// 或 https://host[:port]/path[?query]
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    int timeoutMs = 120000;
    bool insecureTls = false;                    // 跳过证书校验
    /// 流式回调：逐片喂正文（chunked 已解码；TLS 同样生效）；
    /// 返回 false 中止。为空 → 整包读入 body。
    std::function<bool(std::string_view)> onData;
};
struct ZENITH_API SimpleHttpResult {
    bool ok = false;
    int status = 0;
    std::string body;    // 未用 onData 时的完整正文
    std::string error;
    std::string peer;    // "host:port"（诊断）
};
ZENITH_API SimpleHttpResult simpleHttpRequest(const SimpleHttpCall& call);

} // namespace zenith::ai
