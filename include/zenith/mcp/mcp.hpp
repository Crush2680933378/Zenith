// ============================================================================
//  zenith::mcp —— Model Context Protocol（JSON-RPC 2.0，2026-07-28 无状态子集）
//
//  McpServer：注册工具 → 以 JSON-RPC 响应 initialize/tools/list/tools/call。
//  McpClient：对接远端 MCP 服务器（listTools/callTool）。
//  HTTP 挂载见 mcp_api.hpp（POST /mcp，请求体单个对象或数组批）。
//
//  工具 = {name, description, inputSchema} + 处理函数；
//  内置桥接（bridgeTools）可把 ServiceRegistry 里的向量库/推理服务
//  自动包装成 MCP 工具，供 AI Agent（langgraph/claude/zenith::agent）调用。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"

namespace zenith::mcp {

/// JSON-RPC 2.0 标准错误码子集
enum class RpcError : int {
    ParseError = -32700,
    InvalidRequest = -32600,
    MethodNotFound = -32601,
    InvalidParams = -32602,
    InternalError = -32603,
};

struct ToolDef {
    std::string name;
    std::string description;
    json::JsonValue inputSchema = json::JsonValue::object(); // JSON Schema（宽松）
};

struct ToolResult {
    bool isError = false;
    std::string text; // text 内容块（MCP v1 最常用形态）
};

using ToolHandler = std::function<ToolResult(const json::JsonValue& args)>;

/// MCP 服务器（协议基线 2026-07-28 无状态版：无 session、无 streaming）
class ZENITH_API McpServer {
public:
    explicit McpServer(std::string serverName = "zenith",
                       std::string version = "0.1.0");

    /// 注册工具（重名覆盖）
    void registerTool(ToolDef def, ToolHandler handler);

    /// 处理一条 JSON-RPC 请求（对象）→ 响应对象；
    /// 通知（无 id）返回 Null（HTTP 层以 202/空体应答）。
    [[nodiscard]] json::JsonValue handle(const json::JsonValue& request);

    /// HTTP 场景入口：原始请求体（单对象或数组批）→ 响应体字符串
    [[nodiscard]] std::string handleRaw(const std::string& body);

    [[nodiscard]] std::vector<ToolDef> tools() const;
    [[nodiscard]] size_t toolCount() const noexcept { return tools_.size(); }

    /// ---- 内置桥接：把 Zenith 服务包装成 MCP 工具 ----
    /// 向量检索（IVectorStoreService 存在时注册 vector_search / vector_upsert）
    /// 推理（IInferenceService 存在时注册 chat）
    /// 参数均为 ServiceRegistry 原始指针形态（避免本头依赖 plugin 头）。
    void bridgeVector(void* vectorServiceRaw);
    void bridgeInference(void* inferenceServiceRaw);

private:
    json::JsonValue callInitialize(const json::JsonValue& params);
    json::JsonValue callToolsList();
    json::JsonValue callToolsCall(const json::JsonValue& params);

    std::string name_;
    std::string version_;
    std::map<std::string, ToolDef> tools_;
    std::map<std::string, ToolHandler> handlers_;
};

} // namespace zenith::mcp
