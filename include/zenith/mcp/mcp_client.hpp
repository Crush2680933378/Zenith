// ============================================================================
//  zenith::mcp::McpClient —— MCP 客户端（对接远端 MCP 服务器）
//  基于 HttpClient（阻塞形态 fetchBlocking——Agent 循环在独立线程跑时的
//  推荐用法；异步形态可按同协议自行扩展）。
// ============================================================================
#pragma once

#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/json/json.hpp"
#include "zenith/mcp/mcp.hpp"

namespace zenith::mcp {

struct RemoteTool {
    std::string name;
    std::string description;
    json::JsonValue inputSchema;
};

class ZENITH_API McpClient {
public:
    /// baseUrl：远端 MCP HTTP 端点（如 "http://127.0.0.1:8080/mcp"）
    explicit McpClient(std::string baseUrl, int timeoutMs = 15000);

    /// initialize + tools/list → 远端工具清单
    [[nodiscard]] core::Result<std::vector<RemoteTool>> listTools();

    /// tools/call → 文本内容（isError 时 Err）
    [[nodiscard]] core::Result<std::string> callTool(const std::string& name,
                                                     const json::JsonValue& args);

    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    [[nodiscard]] core::Result<json::JsonValue> rpc(const std::string& method,
                                                    const json::JsonValue& params);

    std::string baseUrl_;
    int timeoutMs_;
    int64_t nextId_ = 1;
};

} // namespace zenith::mcp
