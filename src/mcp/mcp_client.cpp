#include "zenith/mcp/mcp_client.hpp"

namespace zenith::mcp {

McpClient::McpClient(std::string baseUrl, int timeoutMs)
    : baseUrl_(std::move(baseUrl)), timeoutMs_(timeoutMs) {}

core::Result<json::JsonValue> McpClient::rpc(const std::string& method,
                                             const json::JsonValue& params) {
    json::JsonValue req = json::JsonValue::object();
    req["jsonrpc"] = "2.0";
    req["id"] = nextId_++;
    req["method"] = method;
    req["params"] = params;

    http::ClientRequest rq;
    rq.url = baseUrl_;
    rq.method = http::HttpMethod::Post;
    rq.body = req.dump();
    rq.headers.set("Content-Type", "application/json");
    rq.timeoutMs = timeoutMs_;
    auto resp = http::HttpClient::fetchBlocking(rq);
    if (!resp) {
        return core::Result<json::JsonValue>::Err(std::move(resp).error());
    }
    auto parsed = json::JsonValue::parse(resp.value().body);
    if (!parsed) {
        return core::Result<json::JsonValue>::Err(
            core::Error(220, "mcp: bad response JSON"));
    }
    json::JsonValue& v = parsed.value();
    if (v["error"].isObject()) {
        return core::Result<json::JsonValue>::Err(
            core::Error(221, "mcp error: " + v["error"]["message"].asString()));
    }
    return core::Result<json::JsonValue>::Ok(v["result"]);
}

core::Result<std::vector<RemoteTool>> McpClient::listTools() {
    // initialize（握手；无状态服务器通常忽略但按协议走一遍）
    {
        json::JsonValue p = json::JsonValue::object();
        p["protocolVersion"] = "2026-07-28";
        p["clientInfo"]["name"] = "zenith-mcp-client";
        auto init = rpc("initialize", p);
        if (!init) return core::Result<std::vector<RemoteTool>>::Err(
                        std::move(init).error());
    }
    auto res = rpc("tools/list", json::JsonValue::object());
    if (!res) return core::Result<std::vector<RemoteTool>>::Err(std::move(res).error());

    std::vector<RemoteTool> out;
    const auto* arr = res.value()["tools"].asArray();
    if (arr == nullptr) return core::Result<std::vector<RemoteTool>>::Ok(std::move(out));
    out.reserve(arr->size());
    for (const json::JsonValue& t : *arr) {
        RemoteTool rt;
        rt.name = t["name"].asString();
        rt.description = t["description"].asString();
        rt.inputSchema = t["inputSchema"];
        out.push_back(std::move(rt));
    }
    return core::Result<std::vector<RemoteTool>>::Ok(std::move(out));
}

core::Result<std::string> McpClient::callTool(const std::string& name,
                                              const json::JsonValue& args) {
    json::JsonValue p = json::JsonValue::object();
    p["name"] = name;
    p["arguments"] = args; // 注：本仓 server 兼容 "args"/"arguments" 两种键
    auto res = rpc("tools/call", p);
    if (!res) return core::Result<std::string>::Err(std::move(res).error());
    json::JsonValue& r = res.value();
    const bool isError = r["isError"].asBool(false);
    const auto* content = r["content"].asArray();
    std::string text;
    if (content != nullptr) {
        for (const json::JsonValue& c : *content) {
            if (c["type"].asString() == "text") {
                if (!text.empty()) text.push_back('\n');
                text += c["text"].asString();
            }
        }
    }
    if (isError) {
        return core::Result<std::string>::Err(core::Error(222, "tool error: " + text));
    }
    return core::Result<std::string>::Ok(std::move(text));
}

} // namespace zenith::mcp
