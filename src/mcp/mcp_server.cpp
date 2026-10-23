#include "zenith/mcp/mcp.hpp"

#include "zenith/ai/inference_service.hpp"
#include "zenith/vector/vector_service.hpp"

namespace zenith::mcp {

McpServer::McpServer(std::string serverName, std::string version)
    : name_(std::move(serverName)), version_(std::move(version)) {}

void McpServer::registerTool(ToolDef def, ToolHandler handler) {
    const std::string n = def.name;
    tools_[n] = std::move(def);
    handlers_[n] = std::move(handler);
}

std::vector<ToolDef> McpServer::tools() const {
    std::vector<ToolDef> out;
    out.reserve(tools_.size());
    for (const auto& [_, t] : tools_) out.push_back(t);
    return out;
}

json::JsonValue McpServer::handle(const json::JsonValue& req) {
    using R = json::JsonValue;
    auto errorResp = [](const json::JsonValue& id, int code, const std::string& msg) {
        R out = R::object();
        out["jsonrpc"] = "2.0";
        out["id"] = id;
        R err = R::object();
        err["code"] = static_cast<int64_t>(code);
        err["message"] = msg;
        out["error"] = std::move(err);
        return out;
    };
    auto okResp = [](const json::JsonValue& id, R result) {
        R out = R::object();
        out["jsonrpc"] = "2.0";
        out["id"] = id;
        out["result"] = std::move(result);
        return out;
    };

    if (!req.isObject()) {
        return errorResp(R(), static_cast<int>(RpcError::InvalidRequest),
                         "request must be an object");
    }
    const json::JsonValue& id = req["id"]; // 缺失 = 通知
    const std::string method = req["method"].asString();
    if (method.empty()) {
        return errorResp(id, static_cast<int>(RpcError::InvalidRequest),
                         "method required");
    }
    const json::JsonValue& params = req["params"];

    json::JsonValue result;
    if (method == "initialize") {
        result = callInitialize(params);
    } else if (method == "ping") {
        result = R::object();
    } else if (method == "tools/list") {
        result = callToolsList();
    } else if (method == "tools/call") {
        result = callToolsCall(params);
    } else if (method == "resources/list") {
        R r = R::object();
        r["resources"] = R::array();
        result = std::move(r);
    } else if (method.rfind("notifications/", 0) == 0) {
        return R(); // 通知：无响应
    } else {
        return errorResp(id, static_cast<int>(RpcError::MethodNotFound),
                         "unknown method: " + method);
    }
    return okResp(id, std::move(result));
}

std::string McpServer::handleRaw(const std::string& body) {
    auto parsed = json::JsonValue::parse(body);
    if (!parsed) {
        json::JsonValue err = json::JsonValue::object();
        err["jsonrpc"] = "2.0";
        err["id"] = json::JsonValue();
        json::JsonValue e = json::JsonValue::object();
        e["code"] = static_cast<int64_t>(RpcError::ParseError);
        e["message"] = "parse error: " + parsed.error().message;
        err["error"] = std::move(e);
        return err.dump();
    }
    json::JsonValue& v = parsed.value();
    if (v.isObject()) {
        const json::JsonValue resp = handle(v);
        return resp.isNull() ? std::string() : resp.dump();
    }
    if (v.isArray()) {
        // 批量：逐条处理，通知跳过；全通知 → 空体
        json::JsonValue out = json::JsonValue::array();
        for (const json::JsonValue& item : *v.asArray()) {
            const json::JsonValue resp = handle(item);
            if (!resp.isNull()) out.push_back(resp);
        }
        return out.empty() ? std::string() : out.dump();
    }
    json::JsonValue err = json::JsonValue::object();
    err["jsonrpc"] = "2.0";
    err["id"] = json::JsonValue();
    json::JsonValue e = json::JsonValue::object();
    e["code"] = static_cast<int64_t>(RpcError::InvalidRequest);
    e["message"] = "request must be object or array";
    err["error"] = std::move(e);
    return err.dump();
}

json::JsonValue McpServer::callInitialize(const json::JsonValue&) {
    json::JsonValue out = json::JsonValue::object();
    out["protocolVersion"] = "2026-07-28";
    json::JsonValue caps = json::JsonValue::object();
    json::JsonValue toolsCap = json::JsonValue::object();
    toolsCap["listChanged"] = false;
    caps["tools"] = std::move(toolsCap);
    out["capabilities"] = std::move(caps);
    json::JsonValue info = json::JsonValue::object();
    info["name"] = name_;
    info["version"] = version_;
    out["serverInfo"] = std::move(info);
    return out;
}

json::JsonValue McpServer::callToolsList() {
    json::JsonValue out = json::JsonValue::object();
    json::JsonValue arr = json::JsonValue::array();
    for (const auto& [_, t] : tools_) {
        json::JsonValue o = json::JsonValue::object();
        o["name"] = t.name;
        o["description"] = t.description;
        o["inputSchema"] = t.inputSchema;
        arr.push_back(std::move(o));
    }
    out["tools"] = std::move(arr);
    return out;
}

json::JsonValue McpServer::callToolsCall(const json::JsonValue& params) {
    json::JsonValue out = json::JsonValue::object();
    const std::string name = params["name"].asString();
    auto it = handlers_.find(name);
    if (it == handlers_.end()) {
        out["isError"] = true;
        json::JsonValue content = json::JsonValue::array();
        json::JsonValue text = json::JsonValue::object();
        text["type"] = "text";
        text["text"] = "no such tool: " + name;
        content.push_back(std::move(text));
        out["content"] = std::move(content);
        return out;
    }
    ToolResult r;
    try {
        // 标准 MCP 键为 "arguments"；兼容本仓历史 "args"
        json::JsonValue args = params["arguments"];
        if (!args.isObject()) args = params["args"];
        r = it->second(args);
    } catch (const std::exception& e) {
        r.isError = true;
        r.text = std::string("tool exception: ") + e.what();
    }
    out["isError"] = r.isError;
    json::JsonValue content = json::JsonValue::array();
    json::JsonValue text = json::JsonValue::object();
    text["type"] = "text";
    text["text"] = r.text.empty() ? "(empty)" : r.text;
    content.push_back(std::move(text));
    out["content"] = std::move(content);
    return out;
}

// ---------------------------------------------------------------------------
// 内置桥接
// ---------------------------------------------------------------------------
void McpServer::bridgeVector(void* vectorServiceRaw) {
    auto* svc = static_cast<vector::IVectorStoreService*>(vectorServiceRaw);
    if (svc == nullptr) return;

    { // vector_search
        ToolDef d;
        d.name = "vector_search";
        d.description =
            "Search the vector database (RAG memory) for the most similar documents. "
            "Returns top-k hits with scores and payloads.";
        json::JsonValue s = json::JsonValue::object();
        s["type"] = "object";
        json::JsonValue props = json::JsonValue::object();
        json::JsonValue col = json::JsonValue::object();
        col["type"] = "string";
        props["collection"] = col;
        json::JsonValue q = json::JsonValue::object();
        q["type"] = "array";
        q["items"]["type"] = "number";
        props["vector"] = q;
        json::JsonValue k = json::JsonValue::object();
        k["type"] = "integer";
        k["default"] = 5;
        props["limit"] = k;
        s["properties"] = props;
        json::JsonValue req = json::JsonValue::array();
        req.push_back("collection");
        req.push_back("vector");
        s["required"] = req;
        d.inputSchema = std::move(s);
        registerTool(std::move(d), [svc](const json::JsonValue& args) {
            const std::string col = args["collection"].asString();
            const auto* arr = args["vector"].asArray();
            if (arr == nullptr) return ToolResult{true, "vector array required"};
            std::vector<float> q;
            q.reserve(arr->size());
            for (const json::JsonValue& e : *arr) {
                q.push_back(static_cast<float>(e.asDouble()));
            }
            auto res = svc->search(col, q, static_cast<int>(args["limit"].asInt(5)));
            if (!res) return ToolResult{true, res.error().message};
            json::JsonValue hits = json::JsonValue::array();
            for (const vector::ScoredPoint& sp : res.value()) {
                json::JsonValue h = json::JsonValue::object();
                h["id"] = static_cast<int64_t>(sp.id);
                h["score"] = static_cast<double>(sp.score);
                h["payload"] = sp.payloadJson.empty()
                                   ? json::JsonValue()
                                   : (json::JsonValue::parse(sp.payloadJson)
                                          ? json::JsonValue::parse(sp.payloadJson).value()
                                          : json::JsonValue());
                hits.push_back(std::move(h));
            }
            return ToolResult{false, hits.dump()};
        });
    }
    { // vector_upsert
        ToolDef d;
        d.name = "vector_upsert";
        d.description = "Store a document into the vector database (RAG long-term memory).";
        json::JsonValue s = json::JsonValue::object();
        s["type"] = "object";
        json::JsonValue props = json::JsonValue::object();
        json::JsonValue col = json::JsonValue::object();
        col["type"] = "string";
        props["collection"] = col;
        json::JsonValue id = json::JsonValue::object();
        id["type"] = "integer";
        props["id"] = id;
        json::JsonValue v = json::JsonValue::object();
        v["type"] = "array";
        v["items"]["type"] = "number";
        props["vector"] = v;
        json::JsonValue pl = json::JsonValue::object();
        pl["type"] = "string";
        props["payload"] = pl;
        s["properties"] = props;
        d.inputSchema = std::move(s);
        registerTool(std::move(d), [svc](const json::JsonValue& args) {
            vector::VectorPoint p;
            p.id = static_cast<uint64_t>(args["id"].asInt());
            const auto* arr = args["vector"].asArray();
            if (arr == nullptr) return ToolResult{true, "vector array required"};
            for (const json::JsonValue& e : *arr) {
                p.vec.push_back(static_cast<float>(e.asDouble()));
            }
            p.payloadJson = args["payload"].asString();
            auto res = svc->upsert(args["collection"].asString(), {p});
            return ToolResult{!static_cast<bool>(res),
                              static_cast<bool>(res) ? "stored" : res.error().message};
        });
    }
}

void McpServer::bridgeInference(void* inferenceServiceRaw) {
    auto* svc = static_cast<ai::IInferenceService*>(inferenceServiceRaw);
    if (svc == nullptr) return;
    ToolDef d;
    d.name = "chat";
    d.description = "Invoke the LLM for a completion (agent-to-agent delegation).";
    json::JsonValue s = json::JsonValue::object();
    s["type"] = "object";
    json::JsonValue props = json::JsonValue::object();
    json::JsonValue prompt = json::JsonValue::object();
    prompt["type"] = "string";
    props["prompt"] = prompt;
    s["properties"] = props;
    d.inputSchema = std::move(s);
    registerTool(std::move(d), [svc](const json::JsonValue& args) {
        ai::ChatRequest req;
        ai::ChatMessage m;
        m.role = "user";
        m.content = args["prompt"].asString();
        req.messages.push_back(std::move(m));
        auto r = svc->chat(req);
        return ToolResult{!r.ok, r.ok ? r.content : r.error};
    });
}

} // namespace zenith::mcp
