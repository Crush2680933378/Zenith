// ============================================================================
//  zenith::agent —— Agent 运行时（M10.3）
//
//  推理-行动循环（ReAct 变体，JSON 协议）：
//    system 提示约定模型每步只输出一个 JSON 对象：
//      {"action":"tool","tool":"名字","args":{...}}   → 执行工具，结果作为
//        observation 回喂，进入下一步
//      {"action":"final","answer":"最终答复"}          → 循环结束
//    解析失败（模型不守约）→ 整段输出视为 final（容错降级）
//
//  工具来源：本地注册（registerTool）+ 远端 MCP（addMcpTools 把
//  McpClient::listTools 的远端工具映射为可调用）。
//
//  记忆：会话滚动窗口（sessionMaxTurns）+ 长期向量召回（可选——
//  提供 IVectorStoreService + 向量化钩子时，每轮 run 前注入 top-k 相关记忆）。
//
//  线程模型：run() 同步阻塞（内部走 IInferenceService::chat 阻塞形态）；
//  建议在 ThreadPool / 独立线程调用，或由宿主以异步任务包装。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "zenith/ai/inference_service.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"
#include "zenith/mcp/mcp_client.hpp"

namespace zenith::agent {

struct AgentConfig {
    std::string model;                       // 空 = 推理服务默认
    std::string systemPrompt;                // 空 = 内置 ReAct 提示
    int maxSteps = 8;                        // 工具调用上限（防死循环）
    int sessionMaxTurns = 20;                // 会话滚动窗口（user/assistant 轮）
    double temperature = 0.2;                // Agent 场景偏低
    // 长期记忆（RAG）：提供时每轮注入相关记忆
    std::string memoryCollection;            // 向量集合名（空 = 不启用）
    int memoryTopK = 3;
    std::function<std::vector<float>(const std::string& text)> embedFn; // 向量化钩子
};

struct AgentStep {
    int step = 0;
    std::string thought;   // 模型原始输出（含 tool JSON 或 final）
    std::string tool;      // 本步执行的工具（final 时空）
    std::string toolArgs;  // args JSON（final 时空）
    std::string observation; // 工具结果（final 时空）
};

struct AgentResult {
    bool ok = false;
    std::string answer;              // 最终答复
    std::vector<AgentStep> steps;    // 全过程（审计/调试）
    int toolCalls = 0;
    ai::Usage usage;                 // 累计 token
    std::string error;
};

class ZENITH_API Agent {
public:
    using ToolFn = std::function<std::string(const json::JsonValue& args)>;

    Agent(std::shared_ptr<ai::IInferenceService> inference, AgentConfig cfg = {});

    /// 注册本地工具（重名覆盖）
    void registerTool(const std::string& name, const std::string& description, ToolFn fn);

    /// 接入远端 MCP 服务器的工具（listTools → 映射为 callTool）
    /// prefix：工具名前缀（避免与本地重名；默认 "remote_"）
    [[nodiscard]] core::Result<size_t> addMcpTools(mcp::McpClient client,
                                                   const std::string& prefix = "remote_");

    /// 执行一轮 Agent 任务（sessionId 空 = 无状态单轮）
    AgentResult run(const std::string& userMessage, const std::string& sessionId = "");

    /// 工具清单（名字 → 描述）——自省/暴露给 MCP
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> toolList() const;

    /// 会话记忆（长期向量）——启用时写入一条记忆
    /// （vec 服务由 embedFn + memoryCollection 推断持有方为调用者注入）
    void remember(void* vectorServiceRaw, const std::string& text, uint64_t id);

    [[nodiscard]] const AgentConfig& config() const noexcept { return cfg_; }

private:
    [[nodiscard]] std::string buildSystemPrompt() const;
    [[nodiscard]] std::vector<ai::ChatMessage> sessionHistory(const std::string& sid);
    void pushTurn(const std::string& sid, const std::string& role, const std::string& content);
    [[nodiscard]] std::string recallMemory(const std::string& query);
    /// 解析模型输出：tool JSON / final JSON / 容错 final
    struct Parsed {
        bool isTool = false;
        std::string tool;
        json::JsonValue args;
        std::string answer;
        bool parsed = false;
    };
    [[nodiscard]] static Parsed parseModelOutput(const std::string& text);

    std::shared_ptr<ai::IInferenceService> inference_;
    AgentConfig cfg_;
    std::map<std::string, ToolFn> tools_;
    std::map<std::string, std::string> toolDescs_;
    std::map<std::string, std::pair<std::string, std::shared_ptr<mcp::McpClient>>> remoteTools_;
    // session → (role, content) 序列
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> sessions_;
    void* vectorSvc_ = nullptr; // remember/recall 的向量服务（宿主注入，弱持有）
};

} // namespace zenith::agent
