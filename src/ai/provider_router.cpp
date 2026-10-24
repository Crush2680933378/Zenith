#include "zenith/ai/provider_router.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>

#include "zenith/core/platform.hpp"
#include "zenith/core/string.hpp"

#if defined(ZENITH_PLATFORM_WINDOWS)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SockFd = SOCKET;
  #define ROUTER_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using SockFd = int;
  #define ROUTER_INVALID (-1)
#endif

namespace zenith::ai {

thread_local std::string ProviderRouter::lastProviderUsed;

const char* providerApiName(ProviderApi api) noexcept {
    switch (api) {
        case ProviderApi::OpenAiChat: return "openai-chat";
        case ProviderApi::Anthropic: return "anthropic";
        case ProviderApi::Gemini: return "gemini";
    }
    return "?";
}

core::Result<ProviderApi> providerApiFrom(const std::string& s) {
    if (s == "openai-chat" || s == "openai-completions" || s == "openai") {
        return core::Result<ProviderApi>::Ok(ProviderApi::OpenAiChat);
    }
    if (s == "anthropic" || s == "anthropic-messages") {
        return core::Result<ProviderApi>::Ok(ProviderApi::Anthropic);
    }
    if (s == "gemini" || s == "gemini-generatecontent") {
        return core::Result<ProviderApi>::Ok(ProviderApi::Gemini);
    }
    return core::Result<ProviderApi>::Err(
        core::Error(400, "unknown provider api: " + s));
}

// ---------------------------------------------------------------------------
// simpleHttpRequest —— 最小阻塞 HTTP 客户端（明文 http://）
// ---------------------------------------------------------------------------
namespace {
struct UrlParts {
    std::string host;
    uint16_t port = 80;
    std::string pathAndQuery = "/";
};

bool parseUrl(const std::string& url, UrlParts& out) {
    const size_t scheme = url.find("://");
    size_t hostBegin = scheme == std::string::npos ? 0 : scheme + 3;
    if (url.compare(0, 7, "http://") == 0) hostBegin = 7;
    if (scheme != std::string::npos && url.compare(0, 8, "https://") == 0) {
        return false; // TLS 上游需前置反代（文件头注释已声明）
    }
    const size_t slash = url.find('/', hostBegin);
    std::string hostPort =
        slash == std::string::npos ? url.substr(hostBegin) : url.substr(hostBegin, slash - hostBegin);
    out.pathAndQuery = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        out.host = hostPort.substr(0, colon);
        out.port = static_cast<uint16_t>(std::atoi(hostPort.c_str() + colon + 1));
    } else {
        out.host = hostPort;
        out.port = 80;
    }
    return !out.host.empty();
}

void closeSock(SockFd s) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    ::closesocket(s);
#else
    ::close(s);
#endif
}
} // namespace

SimpleHttpResult simpleHttpRequest(const SimpleHttpCall& call) {
    SimpleHttpResult out;
    UrlParts parts;
    if (!parseUrl(call.url, parts)) {
        out.error = "https not supported directly (front with a plain-http proxy)";
        return out;
    }

#if defined(ZENITH_PLATFORM_WINDOWS)
    static std::once_flag wsaOnce;
    std::call_once(wsaOnce, [] {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    });
#endif

    const SockFd fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd == ROUTER_INVALID) {
        out.error = "socket() failed";
        return out;
    }
    // 超时（连接与读写）
#if defined(ZENITH_PLATFORM_WINDOWS)
    DWORD tv = static_cast<DWORD>(call.timeoutMs);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = call.timeoutMs / 1000;
    tv.tv_usec = (call.timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (::getaddrinfo(parts.host.c_str(), std::to_string(parts.port).c_str(), &hints,
                      &res) != 0 || res == nullptr) {
        out.error = "dns failed for " + parts.host;
        closeSock(fd);
        return out;
    }
    if (::connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        out.error = "connect failed: " + parts.host + ":" + std::to_string(parts.port);
        freeaddrinfo(res);
        closeSock(fd);
        return out;
    }
    freeaddrinfo(res);

    std::string req;
    req.reserve(256 + call.body.size());
    req += call.method + " " + parts.pathAndQuery + " HTTP/1.1\r\n";
    req += "Host: " + parts.host;
    if (parts.port != 80) req += ":" + std::to_string(parts.port);
    req += "\r\n";
    for (const auto& [k, v] : call.headers) req += k + ": " + v + "\r\n";
    if (!call.body.empty() || call.method == "POST" || call.method == "PUT") {
        req += "Content-Length: " + std::to_string(call.body.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += call.body;

    size_t sent = 0;
    while (sent < req.size()) {
        const int n = ::send(fd, req.data() + sent,
                             static_cast<int>(req.size() - sent), 0);
        if (n <= 0) {
            out.error = "send failed";
            closeSock(fd);
            return out;
        }
        sent += static_cast<size_t>(n);
    }

    std::string all;
    char buf[16384];
    for (;;) {
        const int n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        all.append(buf, static_cast<size_t>(n));
    }
    closeSock(fd);

    // 解析状态行 + 头 + 体（chunked 解码）
    const size_t headEnd = all.find("\r\n\r\n");
    if (headEnd == std::string::npos) {
        out.error = "bad response (no header terminator)";
        return out;
    }
    const std::string head = all.substr(0, headEnd);
    std::string body = all.substr(headEnd + 4);
    const size_t sp = head.find(' ');
    if (sp != std::string::npos) out.status = std::atoi(head.c_str() + sp + 1);

    const std::string lowerHead = core::string_util::toLower(head);
    if (lowerHead.find("transfer-encoding: chunked") != std::string::npos) {
        std::string plain;
        size_t pos = 0;
        while (pos < body.size()) {
            const size_t nl = body.find("\r\n", pos);
            if (nl == std::string::npos) break;
            const long len = strtol(body.substr(pos, nl - pos).c_str(), nullptr, 16);
            if (len <= 0) break;
            plain.append(body, nl + 2, static_cast<size_t>(len));
            pos = nl + 2 + static_cast<size_t>(len) + 2;
        }
        body = std::move(plain);
    }
    out.body = std::move(body);
    out.ok = out.status >= 200 && out.status < 300;
    if (!out.ok) out.error = "http " + std::to_string(out.status);
    return out;
}

// ---------------------------------------------------------------------------
// RouterConfig
// ---------------------------------------------------------------------------
core::Result<RouterConfig> RouterConfig::fromJson(const json::JsonValue& v) {
    if (!v.isObject()) {
        return core::Result<RouterConfig>::Err(core::Error(401, "router config must be object"));
    }
    RouterConfig cfg;
    const auto* provs = v["providers"].asArray();
    if (provs == nullptr) {
        return core::Result<RouterConfig>::Err(core::Error(402, "providers[] required"));
    }
    for (const json::JsonValue& p : *provs) {
        ProviderConfig pc;
        pc.name = p["name"].asString();
        std::string apiStr = p["api"].asString();
        if (apiStr.empty()) apiStr = "openai-chat";
        auto api = providerApiFrom(apiStr);
        if (!api) continue; // 容忍跳过
        pc.api = api.value();
        pc.baseURL = p["baseURL"].asString();
        pc.apiKey = p["apiKey"].asString();
        pc.timeoutMs = static_cast<int>(p["timeoutMs"].asInt(120000));
        if (pc.apiKey.compare(0, 5, "$ENV:") == 0) {
            const char* env = std::getenv(pc.apiKey.c_str() + 5);
            pc.apiKey = env != nullptr ? env : "";
        }
        if (const auto* ms = p["models"].asArray()) {
            for (const json::JsonValue& m : *ms) pc.models.push_back(m.asString());
        }
        if (pc.name.empty() || pc.baseURL.empty()) continue;
        cfg.providers.push_back(std::move(pc));
    }
    if (cfg.providers.empty()) {
        return core::Result<RouterConfig>::Err(core::Error(403, "no valid provider"));
    }
    if (const auto* routes = v["routes"].asObject()) {
        for (const auto& [pattern, target] : *routes) {
            cfg.routes[pattern] = target.asString();
        }
    }
    cfg.defaultProvider = v["defaultProvider"].asString();
    return core::Result<RouterConfig>::Ok(std::move(cfg));
}

// ---------------------------------------------------------------------------
// ProviderRouter
// ---------------------------------------------------------------------------
namespace {
bool wildcardMatch(const std::string& pattern, const std::string& text) {
    // 仅支持尾部 "*" 前缀通配（claude-* → 前缀匹配）
    if (pattern.size() >= 2 && pattern.back() == '*') {
        return text.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;
    }
    return pattern == text;
}
} // namespace

ProviderRouter::ProviderRouter(RouterConfig cfg) : cfg_(std::move(cfg)) {
    for (size_t i = 0; i < cfg_.providers.size(); ++i) {
        byName_[cfg_.providers[i].name] = i;
    }
}

const ProviderConfig* ProviderRouter::route(const std::string& model) const {
    // 1) 精确：厂商 models 清单命中
    for (const ProviderConfig& p : cfg_.providers) {
        for (const std::string& m : p.models) {
            if (m == model) return &p;
        }
    }
    // 2) 通配路由表
    for (const auto& [pattern, target] : cfg_.routes) {
        if (wildcardMatch(pattern, model)) {
            auto it = byName_.find(target);
            if (it != byName_.end()) return &cfg_.providers[it->second];
        }
    }
    // 3) 兜底厂商
    if (!cfg_.defaultProvider.empty()) {
        auto it = byName_.find(cfg_.defaultProvider);
        if (it != byName_.end()) return &cfg_.providers[it->second];
    }
    return nullptr;
}

std::vector<ModelInfo> ProviderRouter::models() {
    std::vector<ModelInfo> out;
    for (const ProviderConfig& p : cfg_.providers) {
        for (const std::string& m : p.models) {
            ModelInfo mi;
            mi.id = m;
            mi.backend = p.name + "/" + providerApiName(p.api);
            out.push_back(std::move(mi));
        }
    }
    return out;
}

EngineState ProviderRouter::engineState() const {
    EngineState st;
    st.running = true;
    return st;
}

ChatResult ProviderRouter::chat(const ChatRequest& req, const TokenCallback&,
                                const TokenCallback&) {
    const std::string model = req.model.empty()
                                  ? (cfg_.defaultProvider.empty() ? ""
                                                                  : cfg_.providers.front().models.empty()
                                                                        ? ""
                                                                        : cfg_.providers.front().models.front())
                                  : req.model;
    const ProviderConfig* p = route(model);
    if (p == nullptr) {
        ChatResult r;
        r.ok = false;
        r.error = "no provider routes model: " + model +
                  " (add to a provider models[] or routes{} or set defaultProvider)";
        return r;
    }
    lastProviderUsed = p->name;
    ChatResult r;
    switch (p->api) {
        case ProviderApi::OpenAiChat: r = chatOpenAi(*p, req); break;
        case ProviderApi::Anthropic: r = chatAnthropic(*p, req); break;
        case ProviderApi::Gemini: r = chatGemini(*p, req); break;
    }
    r.model = model;
    return r;
}

// ---- OpenAI Chat Completions（DeepSeek/OpenAI/Kimi/GLM/SiliconFlow…）----
ChatResult ProviderRouter::chatOpenAi(const ProviderConfig& p, const ChatRequest& req) {
    json::JsonValue body = json::JsonValue::object();
    body["model"] = req.model;
    json::JsonValue msgs = json::JsonValue::array();
    for (const ChatMessage& m : req.messages) {
        json::JsonValue j = json::JsonValue::object();
        j["role"] = m.role;
        j["content"] = m.content;
        msgs.push_back(std::move(j));
    }
    body["messages"] = std::move(msgs);
    body["temperature"] = req.temperature;
    body["max_tokens"] = static_cast<int64_t>(req.maxTokens);
    body["stream"] = false;

    SimpleHttpCall call;
    call.url = p.baseURL + "/chat/completions";
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + p.apiKey}};
    call.timeoutMs = p.timeoutMs;
    auto res = simpleHttpRequest(call);

    ChatResult r;
    if (!res.ok) {
        r.error = "[" + p.name + "] " + res.error + (res.body.empty() ? "" : ": " + res.body.substr(0, 200));
        return r;
    }
    auto v = json::JsonValue::parse(res.body);
    if (!v || !v.value()["choices"].asArray() || v.value()["choices"].asArray()->empty()) {
        r.error = "[" + p.name + "] malformed openai response";
        return r;
    }
    r.ok = true;
    r.content = v.value()["choices"].at(size_t{0})["message"]["content"].asString();
    r.finishReason = v.value()["choices"].at(size_t{0})["finish_reason"].asString();
    if (r.finishReason.empty()) r.finishReason = "stop";
    r.usage.promptTokens = static_cast<int>(v.value()["usage"]["prompt_tokens"].asInt());
    r.usage.completionTokens =
        static_cast<int>(v.value()["usage"]["completion_tokens"].asInt());
    return r;
}

// ---- Anthropic Messages ----
ChatResult ProviderRouter::chatAnthropic(const ProviderConfig& p, const ChatRequest& req) {
    json::JsonValue body = json::JsonValue::object();
    body["model"] = req.model;
    body["max_tokens"] = static_cast<int64_t>(req.maxTokens > 0 ? req.maxTokens : 8192);
    body["temperature"] = req.temperature;
    // system 消息提取为顶层 system；其余原样
    std::string system;
    json::JsonValue msgs = json::JsonValue::array();
    for (const ChatMessage& m : req.messages) {
        if (m.role == "system") {
            if (!system.empty()) system.push_back('\n');
            system += m.content;
            continue;
        }
        json::JsonValue j = json::JsonValue::object();
        j["role"] = m.role;
        j["content"] = m.content;
        msgs.push_back(std::move(j));
    }
    body["messages"] = std::move(msgs);
    if (!system.empty()) body["system"] = system;

    // baseURL 约定到根（https://api.anthropic.com）→ 拼 /v1/messages
    std::string base = p.baseURL;
    while (!base.empty() && base.back() == '/') base.pop_back();
    SimpleHttpCall call;
    call.url = base + "/v1/messages";
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"x-api-key", p.apiKey},
                    {"anthropic-version", "2023-06-01"}};
    call.timeoutMs = p.timeoutMs;
    auto res = simpleHttpRequest(call);

    ChatResult r;
    if (!res.ok) {
        r.error = "[" + p.name + "] " + res.error + (res.body.empty() ? "" : ": " + res.body.substr(0, 200));
        return r;
    }
    auto v = json::JsonValue::parse(res.body);
    if (!v || !v.value()["content"].asArray()) {
        r.error = "[" + p.name + "] malformed anthropic response";
        return r;
    }
    std::string text;
    for (const json::JsonValue& blk : *v.value()["content"].asArray()) {
        if (blk["type"].asString() == "text") {
            if (!text.empty()) text.push_back('\n');
            text += blk["text"].asString();
        }
    }
    r.ok = true;
    r.content = std::move(text);
    r.finishReason = v.value()["stop_reason"].asString();
    if (r.finishReason.empty()) r.finishReason = "stop";
    r.usage.promptTokens = static_cast<int>(v.value()["usage"]["input_tokens"].asInt());
    r.usage.completionTokens = static_cast<int>(v.value()["usage"]["output_tokens"].asInt());
    return r;
}

// ---- Google Gemini generateContent ----
ChatResult ProviderRouter::chatGemini(const ProviderConfig& p, const ChatRequest& req) {
    json::JsonValue body = json::JsonValue::object();
    std::string system;
    json::JsonValue contents = json::JsonValue::array();
    for (const ChatMessage& m : req.messages) {
        if (m.role == "system") {
            if (!system.empty()) system.push_back('\n');
            system += m.content;
            continue;
        }
        json::JsonValue c = json::JsonValue::object();
        c["role"] = m.role == "assistant" ? "model" : "user";
        json::JsonValue parts = json::JsonValue::array();
        json::JsonValue t = json::JsonValue::object();
        t["text"] = m.content;
        parts.push_back(std::move(t));
        c["parts"] = std::move(parts);
        contents.push_back(std::move(c));
    }
    body["contents"] = std::move(contents);
    if (!system.empty()) {
        json::JsonValue si = json::JsonValue::object();
        json::JsonValue parts = json::JsonValue::array();
        json::JsonValue t = json::JsonValue::object();
        t["text"] = system;
        parts.push_back(std::move(t));
        si["parts"] = std::move(parts);
        body["systemInstruction"] = std::move(si);
    }
    json::JsonValue gen = json::JsonValue::object();
    gen["temperature"] = req.temperature;
    if (req.maxTokens > 0) gen["maxOutputTokens"] = static_cast<int64_t>(req.maxTokens);
    body["generationConfig"] = std::move(gen);

    std::string base = p.baseURL;
    while (!base.empty() && base.back() == '/') base.pop_back();
    SimpleHttpCall call;
    call.url = base + "/models/" + req.model + ":generateContent";
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"x-goog-api-key", p.apiKey}};
    call.timeoutMs = p.timeoutMs;
    auto res = simpleHttpRequest(call);

    ChatResult r;
    if (!res.ok) {
        r.error = "[" + p.name + "] " + res.error + (res.body.empty() ? "" : ": " + res.body.substr(0, 200));
        return r;
    }
    auto v = json::JsonValue::parse(res.body);
    const auto* cands = v ? v.value()["candidates"].asArray() : nullptr;
    if (cands == nullptr || cands->empty()) {
        r.error = "[" + p.name + "] malformed gemini response";
        return r;
    }
    std::string text;
    if (const auto* parts = cands->at(size_t{0})["content"]["parts"].asArray()) {
        for (const json::JsonValue& part : *parts) {
            if (!text.empty()) text.push_back('\n');
            text += part["text"].asString();
        }
    }
    r.ok = true;
    r.content = std::move(text);
    r.finishReason = cands->at(size_t{0})["finishReason"].asString();
    if (r.finishReason.empty()) r.finishReason = "STOP";
    r.usage.promptTokens =
        static_cast<int>(v.value()["usageMetadata"]["promptTokenCount"].asInt());
    r.usage.completionTokens =
        static_cast<int>(v.value()["usageMetadata"]["candidatesTokenCount"].asInt());
    return r;
}

EmbeddingResult ProviderRouter::embeddings(const std::string& model,
                                           const std::string& input) {
    EmbeddingResult r;
    const ProviderConfig* p = route(model);
    if (p == nullptr || p->api != ProviderApi::OpenAiChat) {
        r.error = "embeddings only via openai-chat providers (model: " + model + ")";
        return r;
    }
    json::JsonValue body = json::JsonValue::object();
    body["model"] = model;
    body["input"] = input;
    SimpleHttpCall call;
    call.url = p->baseURL + "/embeddings";
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + p->apiKey}};
    call.timeoutMs = p->timeoutMs;
    auto res = simpleHttpRequest(call);
    if (!res.ok) {
        r.error = "[" + p->name + "] " + res.error;
        return r;
    }
    auto v = json::JsonValue::parse(res.body);
    const auto* arr = v ? v.value()["data"].asArray() : nullptr;
    if (arr == nullptr || arr->empty()) {
        r.error = "malformed embeddings response";
        return r;
    }
    if (const auto* vec = arr->at(size_t{0})["embedding"].asArray()) {
        for (const json::JsonValue& e : *vec) {
            r.vector.push_back(static_cast<float>(e.asDouble()));
        }
    }
    r.ok = !r.vector.empty();
    r.model = model;
    if (!r.ok) r.error = "empty embedding";
    return r;
}

core::Result<std::vector<std::string>> ProviderRouter::discoverModels(
    const std::string& providerName) {
    auto it = byName_.find(providerName);
    if (it == byName_.end()) {
        return core::Result<std::vector<std::string>>::Err(
            core::Error(404, "no such provider"));
    }
    const ProviderConfig& p = cfg_.providers[it->second];
    if (p.api != ProviderApi::OpenAiChat) {
        // 非 openai 协议：返回配置内清单（anthropic models 列表协议不同——
        // 与 DeepSeek Harness 现状一致的取舍，见 discussion #2529）
        return core::Result<std::vector<std::string>>::Ok(p.models);
    }
    SimpleHttpCall call;
    call.method = "GET";
    call.url = p.baseURL + "/models";
    call.headers = {{"Authorization", "Bearer " + p.apiKey}};
    call.timeoutMs = p.timeoutMs;
    auto res = simpleHttpRequest(call);
    if (!res.ok) {
        return core::Result<std::vector<std::string>>::Err(
            core::Error(405, res.error + ": " + res.body.substr(0, 120)));
    }
    auto v = json::JsonValue::parse(res.body);
    const auto* data = v ? v.value()["data"].asArray() : nullptr;
    if (data == nullptr) {
        return core::Result<std::vector<std::string>>::Err(
            core::Error(406, "malformed /models response"));
    }
    std::vector<std::string> out;
    for (const json::JsonValue& m : *data) out.push_back(m["id"].asString());
    return core::Result<std::vector<std::string>>::Ok(std::move(out));
}

} // namespace zenith::ai
