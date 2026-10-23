#include "zenith/agent/agent.hpp"

#include "zenith/vector/vector_service.hpp"

namespace zenith::agent {

namespace {
const char* kDefaultSystemPrompt =
    "You are a helpful agent that can use tools to complete tasks.\n"
    "On each turn you MUST reply with EXACTLY ONE JSON object and nothing else:\n"
    "  To call a tool: {\"action\":\"tool\",\"tool\":\"<name>\",\"args\":{...}}\n"
    "  To finish:      {\"action\":\"final\",\"answer\":\"<your final answer>\"}\n"
    "Rules:\n"
    "- Use tools when they help; do not call a tool if you already know the answer.\n"
    "- After each tool call you will receive its observation; continue reasoning.\n"
    "- Finish with \"final\" as soon as the user's request is satisfied.\n"
    "- Never wrap the JSON in markdown or extra text.";
} // namespace

Agent::Agent(std::shared_ptr<ai::IInferenceService> inference, AgentConfig cfg)
    : inference_(std::move(inference)), cfg_(std::move(cfg)) {}

void Agent::registerTool(const std::string& name, const std::string& description,
                         ToolFn fn) {
    tools_[name] = std::move(fn);
    toolDescs_[name] = description;
}

core::Result<size_t> Agent::addMcpTools(mcp::McpClient client, const std::string& prefix) {
    auto list = client.listTools();
    if (!list) return core::Result<size_t>::Err(std::move(list).error());
    size_t n = 0;
    for (mcp::RemoteTool& t : list.value()) {
        const std::string localName = prefix + t.name;
        auto cli = std::make_shared<mcp::McpClient>(client.baseUrl(), 15000);
        remoteTools_[localName] = {t.name, cli};
        toolDescs_[localName] = "[remote mcp] " + t.description;
        ++n;
    }
    return core::Result<size_t>::Ok(n);
}

std::vector<std::pair<std::string, std::string>> Agent::toolList() const {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [name, desc] : toolDescs_) out.emplace_back(name, desc);
    return out;
}

std::string Agent::buildSystemPrompt() const {
    std::string sp = cfg_.systemPrompt.empty() ? kDefaultSystemPrompt : cfg_.systemPrompt;
    if (!toolDescs_.empty()) {
        sp += "\n\nAvailable tools:";
        for (const auto& [name, desc] : toolDescs_) {
            sp += "\n- " + name + ": " + desc;
        }
    }
    return sp;
}

std::vector<ai::ChatMessage> Agent::sessionHistory(const std::string& sid) {
    std::vector<ai::ChatMessage> out;
    if (sid.empty()) return out;
    auto it = sessions_.find(sid);
    if (it == sessions_.end()) return out;
    for (const auto& [role, content] : it->second) {
        ai::ChatMessage m;
        m.role = role;
        m.content = content;
        out.push_back(std::move(m));
    }
    return out;
}

void Agent::pushTurn(const std::string& sid, const std::string& role,
                     const std::string& content) {
    if (sid.empty()) return;
    auto& hist = sessions_[sid];
    hist.emplace_back(role, content);
    // 滚动窗口：超限丢最旧（成对丢弃由 user/assistant 标记保证平衡性近似）
    if (static_cast<int>(hist.size()) > cfg_.sessionMaxTurns * 2) {
        hist.erase(hist.begin(), hist.begin() + static_cast<long>(hist.size()) -
                                     static_cast<long>(cfg_.sessionMaxTurns * 2));
    }
}

std::string Agent::recallMemory(const std::string& query) {
    if (cfg_.memoryCollection.empty() || !cfg_.embedFn || vectorSvc_ == nullptr) {
        return {};
    }
    auto* vec = static_cast<vector::IVectorStoreService*>(vectorSvc_);
    const std::vector<float> q = cfg_.embedFn(query);
    if (q.empty()) return {};
    auto res = vec->search(cfg_.memoryCollection, q, cfg_.memoryTopK);
    if (!res || res.value().empty()) return {};
    std::string context = "\n\nRelevant long-term memories:\n";
    for (const vector::ScoredPoint& sp : res.value()) {
        if (sp.payloadJson.empty()) continue;
        context += "- " + sp.payloadJson + "\n";
    }
    return context;
}

void Agent::remember(void* vectorServiceRaw, const std::string& text, uint64_t id) {
    vectorSvc_ = vectorServiceRaw;
    if (cfg_.memoryCollection.empty() || !cfg_.embedFn || vectorServiceRaw == nullptr) {
        return;
    }
    auto* vec = static_cast<vector::IVectorStoreService*>(vectorServiceRaw);
    const std::vector<float> v = cfg_.embedFn(text);
    if (v.empty()) return;
    vector::VectorPoint p;
    p.id = id;
    p.vec = v;
    p.payloadJson = text;
    (void)vec->upsert(cfg_.memoryCollection, {p});
}

Agent::Parsed Agent::parseModelOutput(const std::string& text) {
    Parsed p;
    // 定位首尾大括号——容忍模型在 JSON 前后夹说明文字
    const size_t b = text.find('{');
    const size_t e = text.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b) return p;
    auto v = json::JsonValue::parse(text.substr(b, e - b + 1));
    if (!v || !v.value().isObject()) return p;
    const json::JsonValue& obj = v.value();
    const std::string action = obj["action"].asString();
    if (action == "tool") {
        p.parsed = true;
        p.isTool = true;
        p.tool = obj["tool"].asString();
        p.args = obj["args"];
        if (p.tool.empty()) p.parsed = false;
    } else if (action == "final") {
        p.parsed = true;
        p.answer = obj["answer"].asString();
    }
    return p;
}

AgentResult Agent::run(const std::string& userMessage, const std::string& sessionId) {
    AgentResult result;
    if (inference_ == nullptr) {
        result.error = "no inference service";
        return result;
    }

    // 组装消息：system + 长期记忆 + 会话历史 + 本轮输入
    std::vector<ai::ChatMessage> messages;
    {
        ai::ChatMessage sys;
        sys.role = "system";
        sys.content = buildSystemPrompt();
        messages.push_back(std::move(sys));
    }
    if (const std::string mem = recallMemory(userMessage); !mem.empty()) {
        ai::ChatMessage m;
        m.role = "system";
        m.content = mem;
        messages.push_back(std::move(m));
    }
    for (ai::ChatMessage& m : sessionHistory(sessionId)) {
        messages.push_back(std::move(m));
    }
    {
        ai::ChatMessage u;
        u.role = "user";
        u.content = userMessage;
        messages.push_back(std::move(u));
    }

    for (int step = 1; step <= cfg_.maxSteps; ++step) {
        ai::ChatRequest req;
        req.model = cfg_.model;
        req.temperature = cfg_.temperature;
        req.messages = messages;
        auto r = inference_->chat(req);
        if (!r.ok) {
            result.error = "inference failed: " + r.error;
            return result;
        }
        result.usage.promptTokens += r.usage.promptTokens;
        result.usage.completionTokens += r.usage.completionTokens;

        const Parsed parsed = parseModelOutput(r.content);
        AgentStep s;
        s.step = step;
        s.thought = r.content;

        if (parsed.parsed && parsed.isTool) {
            s.tool = parsed.tool;
            s.toolArgs = parsed.args.dump();

            // 执行：本地 → 远端 MCP → 未知
            std::string observation;
            auto lit = tools_.find(parsed.tool);
            if (lit != tools_.end()) {
                try {
                    observation = lit->second(parsed.args);
                } catch (const std::exception& e) {
                    observation = std::string("tool error: ") + e.what();
                }
            } else {
                auto rit = remoteTools_.find(parsed.tool);
                if (rit != remoteTools_.end()) {
                    auto call = rit->second.second->callTool(rit->second.first, parsed.args);
                    observation = call ? call.value() : ("remote tool error: " +
                                                         call.error().message);
                } else {
                    observation = "unknown tool: " + parsed.tool;
                }
            }
            s.observation = observation;
            result.steps.push_back(std::move(s));
            ++result.toolCalls;

            // observation 以 user 角色回喂（tool 角色本仓 ChatMessage 未建模）
            ai::ChatMessage ob;
            ob.role = "user";
            ob.content = "Tool observation (" + parsed.tool + "): " + observation;
            messages.push_back(std::move(ob));
            continue;
        }

        // final（守约或容错降级）
        s.tool.clear();
        s.observation = "";
        result.steps.push_back(std::move(s));
        result.ok = true;
        result.answer = parsed.parsed ? parsed.answer : r.content;
        pushTurn(sessionId, "user", userMessage);
        pushTurn(sessionId, "assistant", result.answer);
        return result;
    }
    result.error = "max steps exceeded (" + std::to_string(cfg_.maxSteps) + ")";
    return result;
}

} // namespace zenith::agent
