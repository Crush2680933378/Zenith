// ============================================================================
//  zenith::mcp::mountMcpApi —— MCP over HTTP 挂载（M10.2）
//
//  挂载（宿主持 Router 与 McpServer）：
//      mcp::mountMcpApi(server.router(), mcpServer);
//
//  端点：
//    POST /mcp      JSON-RPC 2.0（单对象或数组批；通知 → 202 空体）
//    GET  /mcp      端点自述（人类可读的服务器信息 + 工具名清单）
// ============================================================================
#pragma once

#include <memory>

#include "zenith/http/http_server.hpp"
#include "zenith/mcp/mcp.hpp"

namespace zenith::mcp {

inline void mountMcpApi(http::Router& r, std::shared_ptr<McpServer> server) {
    using namespace http;

    r.post("/mcp", [server](Context& c) {
        const std::string body = server->handleRaw(c.body());
        if (body.empty()) {
            c.status(202).noContent(); // 全通知（含 notifications/initialized）
            return;
        }
        c.setHeader("Content-Type", "application/json");
        c.status(200);
        c.response().body = body;
    });

    r.get("/mcp", [server](Context& c) {
        json::JsonValue v = json::JsonValue::object();
        v["server"] = "zenith-mcp";
        v["protocol"] = "2026-07-28";
        json::JsonValue names = json::JsonValue::array();
        for (const ToolDef& t : server->tools()) names.push_back(t.name);
        v["tools"] = std::move(names);
        c.json(200, v);
    });
}

} // namespace zenith::mcp
