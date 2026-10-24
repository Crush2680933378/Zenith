#include "zenith/ai/provider_router.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>

#include "zenith/core/platform.hpp"
#include "zenith/core/string.hpp"

#if defined(ZENITH_TLS_OPENSSL)
  #include <openssl/err.h>
  #include <openssl/ssl.h>
#endif

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
    // https 由 simpleHttpRequest 的 TLS 路径处理（ZENITH_TLS_OPENSSL 门控）
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
    const bool tls = call.url.compare(0, 8, "https://") == 0;
    if (call.url.compare(0, 7, "http://") != 0 && !tls) {
        out.error = "only http:// and https:// supported";
        return out;
    }
    if (!parseUrl(call.url, parts)) {
        out.error = "invalid url";
        return out;
    }
    if (tls && parts.port == 80) parts.port = 443;
    out.peer = parts.host + ":" + std::to_string(parts.port);

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
#if defined(ZENITH_PLATFORM_WINDOWS)
    DWORD tv = static_cast<DWORD>(call.timeoutMs);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    DWORD ctv = static_cast<DWORD>(call.timeoutMs < 15000 ? call.timeoutMs : 15000);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ctv), sizeof(ctv));
#else
    timeval tv{};
    tv.tv_sec = call.timeoutMs / 1000;
    tv.tv_usec = (call.timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    timeval ctv{};
    ctv.tv_sec = (call.timeoutMs < 15000 ? call.timeoutMs : 15000) / 1000;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
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
        out.error = "connect failed: " + out.peer;
        freeaddrinfo(res);
        closeSock(fd);
        return out;
    }
    freeaddrinfo(res);

    // ---- TLS 包裹（阻塞 OpenSSL；SNI + 主机名校验；insecure 跳过校验）----
#if defined(ZENITH_TLS_OPENSSL)
    SSL_CTX* ownCtx = nullptr;  // insecure 专用（独立 ctx 免串扰）
    SSL* ssl = nullptr;
    if (tls) {
        static SSL_CTX* sharedCtx = [] {
            SSL_CTX* c = SSL_CTX_new(TLS_client_method());
            if (c != nullptr) {
                SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
                SSL_CTX_set_default_verify_paths(c);
                SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr); // 证书校验失败必须致命
                SSL_CTX_set_options(c, SSL_OP_NO_COMPRESSION);
            }
            return c;
        }();
        SSL_CTX* ctx = sharedCtx;
        if (call.insecureTls) {
            ownCtx = SSL_CTX_new(TLS_client_method());
            if (ownCtx != nullptr) SSL_CTX_set_verify(ownCtx, SSL_VERIFY_NONE, nullptr);
            ctx = ownCtx;
        }
        ssl = ctx != nullptr ? SSL_new(ctx) : nullptr;
        if (ssl == nullptr) {
            out.error = "tls ctx setup failed";
            if (ownCtx != nullptr) SSL_CTX_free(ownCtx);
            closeSock(fd);
            return out;
        }
        SSL_set_fd(ssl, static_cast<int>(fd));
#if defined(__GNUC__)
// 宏内自带 C 风格转换（OpenSSL 头），局部豁免 -Wold-style-cast
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif
        SSL_set_tlsext_host_name(ssl, parts.host.c_str()); // SNI
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
        if (!call.insecureTls) {
            SSL_set1_host(ssl, parts.host.c_str());         // 证书主机名校验
        }
        if (SSL_connect(ssl) != 1) {
            out.error = std::string("tls handshake failed: ") +
                        ERR_reason_error_string(ERR_get_error());
            SSL_free(ssl);
            if (ownCtx != nullptr) SSL_CTX_free(ownCtx);
            closeSock(fd);
            return out;
        }
    }
#else
    if (tls) {
        out.error = "https requires ZENITH_TLS=openssl build";
        closeSock(fd);
        return out;
    }
#endif

    auto sendFn = [&](const char* data, size_t n) -> int {
#if defined(ZENITH_TLS_OPENSSL)
        if (ssl != nullptr) return SSL_write(ssl, data, static_cast<int>(n));
#endif
        return ::send(fd, data, static_cast<int>(n), 0);
    };
    auto recvFn = [&](char* data, size_t n) -> int {
#if defined(ZENITH_TLS_OPENSSL)
        if (ssl != nullptr) return SSL_read(ssl, data, static_cast<int>(n));
#endif
        return ::recv(fd, data, static_cast<int>(n), 0);
    };

    std::string req;
    req.reserve(256 + call.body.size());
    req += call.method + " " + parts.pathAndQuery + " HTTP/1.1\r\n";
    req += "Host: " + parts.host;
    if ((!tls && parts.port != 80) || (tls && parts.port != 443)) {
        req += ":" + std::to_string(parts.port);
    }
    req += "\r\n";
    for (const auto& [k, v] : call.headers) req += k + ": " + v + "\r\n";
    if (!call.body.empty() || call.method == "POST" || call.method == "PUT") {
        req += "Content-Length: " + std::to_string(call.body.size()) + "\r\n";
    }
    req += "Connection: close\r\n\r\n";
    req += call.body;

    size_t sent = 0;
    while (sent < req.size()) {
        const int n = sendFn(req.data() + sent, req.size() - sent);
        if (n <= 0) {
            out.error = "send failed";
#if defined(ZENITH_TLS_OPENSSL)
            if (ssl != nullptr) {
                SSL_free(ssl);
                if (ownCtx != nullptr) SSL_CTX_free(ownCtx);
            }
#endif
            closeSock(fd);
            return out;
        }
        sent += static_cast<size_t>(n);
    }

    // ---- 收包：onData 流式（原始字节，SSE 解析归调用方）或整包 ----
    std::string all;
    char buf[16384];
    bool aborted = false;
    for (;;) {
        const int n = recvFn(buf, sizeof(buf));
        if (n <= 0) break;
        if (call.onData) {
            if (!call.onData(std::string_view(buf, static_cast<size_t>(n)))) {
                aborted = true;
                break;
            }
        } else {
            all.append(buf, static_cast<size_t>(n));
        }
    }
#if defined(ZENITH_TLS_OPENSSL)
    if (ssl != nullptr) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        if (ownCtx != nullptr) SSL_CTX_free(ownCtx);
    }
#endif
    closeSock(fd);

    if (call.onData) {
        out.ok = !aborted; // 状态行解析归调用方（流式契约见头注释）
        return out;
    }
    if (aborted) {
        out.ok = false;
        return out;
    }

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
// SseTranslator —— 上游流式响应 → 统一 token 增量（M13.3）
//  输入：simpleHttpRequest onData 的原始字节（状态行+头+chunked 或明文 body）
//  行为：增量解码 chunked、按 SSE 帧切事件、按协议方言抽取 delta 文本，
//        即时回调 onToken/onReasoning；同时累积全文与 usage/finish 元数据。
// ---------------------------------------------------------------------------
namespace {
struct SseTranslator {
    ProviderApi api;
    std::string* contentOut;
    std::string* reasoningOut;
    const TokenCallback* onToken;
    const TokenCallback* onReasoning;

    SseTranslator(ProviderApi a, std::string* content, std::string* reasoning,
                  const TokenCallback* tok, const TokenCallback* rea)
        : api(a), contentOut(content), reasoningOut(reasoning), onToken(tok),
          onReasoning(rea) {}

    bool headOk = false;
    bool headDone = false;
    bool chunked = false;
    bool chunkDone = false;
    bool wantChunkHeader = true;
    size_t chunkRemain = 0;
    bool sawAny = false;   // 至少翻译出一个 delta
    std::string buf;       // 未消费原始字节
    std::string ev;        // 当前 SSE 事件（含换行）
    std::string data;      // 当前事件 data 字段（多行 '\n' 连接）
    std::string error;
    std::string finish;
    int promptTokens = -1;
    int completionTokens = -1;

    bool emitToken(const std::string& t) {
        sawAny = true;
        *contentOut += t;
        return onToken == nullptr || (*onToken)(t);
    }
    bool emitReasoning(const std::string& t) {
        sawAny = true;
        *reasoningOut += t;
        return onReasoning == nullptr || (*onReasoning)(t);
    }

    /// 返回 false = 中止（onToken 拒绝或非 2xx）
    bool feed(std::string_view raw) {
        buf.append(raw.data(), raw.size());
        if (!headDone) {
            const size_t eoh = buf.find("\r\n\r\n");
            if (eoh == std::string::npos) return true;
            const std::string head = buf.substr(0, eoh);
            buf.erase(0, eoh + 4);
            const size_t sp = head.find(' ');
            const int status = sp == std::string::npos ? 0 : std::atoi(head.c_str() + sp + 1);
            headOk = status >= 200 && status < 300;
            chunked = core::string_util::toLower(head).find(
                          "transfer-encoding: chunked") != std::string::npos;
            headDone = true;
            if (!headOk) {
                error = "http " + std::to_string(status) + ": " + buf.substr(0, 200);
                return false;
            }
        }
        std::string plain;
        if (chunked) {
            while (!chunkDone) {
                if (wantChunkHeader) {
                    const size_t nl = buf.find("\r\n");
                    if (nl == std::string::npos) break;
                    const long len = std::strtol(buf.substr(0, nl).c_str(), nullptr, 16);
                    buf.erase(0, nl + 2);
                    if (len <= 0) {
                        chunkDone = true;
                        break;
                    }
                    chunkRemain = static_cast<size_t>(len);
                    wantChunkHeader = false;
                }
                if (buf.size() < chunkRemain + 2) break; // 等整块到齐（含尾 \r\n）
                plain.append(buf, 0, chunkRemain);
                buf.erase(0, chunkRemain + 2);
                wantChunkHeader = true;
            }
        } else {
            plain = std::move(buf);
            buf.clear();
        }
        for (char ch : plain) {
            ev.push_back(ch);
            if (ev.size() >= 2 && ev[ev.size() - 2] == '\n' && ev.back() == '\n') {
                ev.pop_back();
                ev.pop_back();
                if (!dispatchEvent()) return false;
                ev.clear();
            }
        }
        return true;
    }

    bool dispatchEvent() {
        data.clear();
        size_t pos = 0;
        while (pos <= ev.size()) {
            const size_t nl = ev.find('\n', pos);
            std::string line = ev.substr(pos, nl == std::string::npos ? ev.size() - pos
                                                                      : nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.compare(0, 5, "data:") == 0) {
                size_t off = 5;
                if (off < line.size() && line[off] == ' ') ++off;
                if (!data.empty()) data.push_back('\n');
                data.append(line, off, std::string::npos);
            } // event:/id:/retry:/注释行 —— 方言信息都在 data JSON 里，忽略
            if (nl == std::string::npos) break;
            pos = nl + 1;
        }
        if (data.empty()) return true;
        if (data == "[DONE]") return true;
        auto v = json::JsonValue::parse(data);
        if (!v) return true; // 容忍坏帧
        switch (api) {
            case ProviderApi::OpenAiChat: {
                if (const auto* chs = v.value()["choices"].asArray()) {
                    if (!chs->empty()) {
                        const json::JsonValue& d = chs->at(size_t{0})["delta"];
                        std::string rc = d["reasoning_content"].asString();
                        if (!rc.empty() && !emitReasoning(rc)) return false;
                        std::string t = d["content"].asString();
                        if (!t.empty() && !emitToken(t)) return false;
                        std::string fr = chs->at(size_t{0})["finish_reason"].asString();
                        if (!fr.empty()) finish = fr;
                    }
                }
                if (v.value()["usage"].isObject()) {
                    int pt = static_cast<int>(v.value()["usage"]["prompt_tokens"].asInt(-1));
                    int ct = static_cast<int>(v.value()["usage"]["completion_tokens"].asInt(-1));
                    if (pt >= 0) promptTokens = pt;
                    if (ct >= 0) completionTokens = ct;
                }
                break;
            }
            case ProviderApi::Anthropic: {
                const std::string type = v.value()["type"].asString();
                if (type == "content_block_delta") {
                    const json::JsonValue& d = v.value()["delta"];
                    const std::string dt = d["type"].asString();
                    if (dt == "text_delta") {
                        std::string t = d["text"].asString();
                        if (!t.empty() && !emitToken(t)) return false;
                    } else if (dt == "thinking_delta") {
                        std::string t = d["thinking"].asString();
                        if (!t.empty() && !emitReasoning(t)) return false;
                    }
                } else if (type == "message_delta") {
                    std::string fr = v.value()["delta"]["stop_reason"].asString();
                    if (!fr.empty()) finish = fr;
                    int ot = static_cast<int>(v.value()["usage"]["output_tokens"].asInt(-1));
                    if (ot >= 0) completionTokens = ot;
                }
                break;
            }
            case ProviderApi::Gemini: {
                if (const auto* cands = v.value()["candidates"].asArray()) {
                    if (!cands->empty()) {
                        if (const auto* parts =
                                cands->at(size_t{0})["content"]["parts"].asArray()) {
                            for (const json::JsonValue& part : *parts) {
                                std::string t = part["text"].asString();
                                if (!t.empty() && !emitToken(t)) return false;
                            }
                        }
                        std::string fr = cands->at(size_t{0})["finishReason"].asString();
                        if (!fr.empty()) finish = fr;
                    }
                }
                break;
            }
        }
        return true;
    }
};
} // namespace

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
        pc.insecure = p["insecure"].asBool(false);
        pc.fallbackTo = p["fallbackTo"].asString();
        pc.cbFailThreshold = static_cast<int>(p["circuit"]["failThreshold"].asInt(3));
        pc.cbOpenMs = static_cast<int>(p["circuit"]["openMs"].asInt(30000));
        pc.pricePromptPerM = p["pricing"]["promptPerM"].asDouble(0.0);
        pc.priceCompletionPerM = p["pricing"]["completionPerM"].asDouble(0.0);
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
    rt_ = std::make_unique<Rt>(cfg_.providers.size());
}

// ---- 运行态：熔断 + 统计（M13 第二批）----
struct ProviderRouter::Rt {
    struct Per {
        uint64_t calls = 0;
        uint64_t failures = 0;
        uint64_t fallbacks = 0;
        uint64_t tokensIn = 0;
        uint64_t tokensOut = 0;
        double costUsd = 0.0;
        uint64_t consecutiveFails = 0;
        bool open = false;
        bool halfOpenProbe = false;      // 冷却后放行的单次试探
        std::chrono::steady_clock::time_point openUntil{};
    };
    std::mutex mtx;
    std::vector<Per> per;

    explicit Rt(size_t n) : per(n) {}

    /// 熔断闸门：false = 拒绝（open 且未到冷却）。半开只放一次试探。
    bool gate(size_t idx, const ProviderConfig& p,
              std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lk(mtx);
        Per& s = per[idx];
        if (!s.open) return true;
        if (now < s.openUntil) return false;
        if (s.halfOpenProbe) return false; // 已有试探在飞
        s.halfOpenProbe = true;            // 放行一次
        return true;
    }
    void onSuccess(size_t idx, const ProviderConfig& p, int tokensIn, int tokensOut) {
        std::lock_guard<std::mutex> lk(mtx);
        Per& s = per[idx];
        ++s.calls;
        s.consecutiveFails = 0;
        s.open = false;
        s.halfOpenProbe = false;
        s.tokensIn += static_cast<uint64_t>(tokensIn > 0 ? tokensIn : 0);
        s.tokensOut += static_cast<uint64_t>(tokensOut > 0 ? tokensOut : 0);
        if (p.pricePromptPerM > 0 || p.priceCompletionPerM > 0) {
            s.costUsd += static_cast<double>(tokensIn > 0 ? tokensIn : 0) / 1e6 *
                             p.pricePromptPerM +
                         static_cast<double>(tokensOut > 0 ? tokensOut : 0) / 1e6 *
                             p.priceCompletionPerM;
        }
    }
    void onFailure(size_t idx, const ProviderConfig& p,
                   std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lk(mtx);
        Per& s = per[idx];
        ++s.calls;
        ++s.failures;
        ++s.consecutiveFails;
        s.halfOpenProbe = false;
        if (p.cbFailThreshold > 0 && s.consecutiveFails >=
                                         static_cast<uint64_t>(p.cbFailThreshold)) {
            s.open = true;
            s.openUntil = now + std::chrono::milliseconds(p.cbOpenMs);
        }
    }
    void onFallback(size_t idx) {
        std::lock_guard<std::mutex> lk(mtx);
        ++per[idx].fallbacks;
    }
};

ProviderRouter::~ProviderRouter() = default;

std::map<std::string, ProviderRouter::ProviderStat> ProviderRouter::statsSnapshot() const {
    std::lock_guard<std::mutex> lk(rt_->mtx);
    std::map<std::string, ProviderStat> out;
    for (size_t i = 0; i < cfg_.providers.size() && i < rt_->per.size(); ++i) {
        const Rt::Per& s = rt_->per[i];
        ProviderStat st;
        st.calls = s.calls;
        st.failures = s.failures;
        st.fallbacks = s.fallbacks;
        st.tokensIn = s.tokensIn;
        st.tokensOut = s.tokensOut;
        st.costUsd = s.costUsd;
        st.cbState = s.open
                         ? (std::chrono::steady_clock::now() < s.openUntil ? "open" : "half-open")
                         : "closed";
        out[cfg_.providers[i].name] = st;
    }
    return out;
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

ChatResult ProviderRouter::chat(const ChatRequest& req, const TokenCallback& onToken,
                                const TokenCallback& onReasoning) {
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

    // failover 链：主 → fallbackTo → fallbackTo…（环路由防御：只走一遍去重链）
    ChatResult r;
    std::vector<size_t> visited;
    size_t idx = byName_.at(p->name);
    for (int hop = 0; hop < 8; ++hop) {
        const ProviderConfig& cur = cfg_.providers[idx];
        bool seen = false;
        for (size_t v : visited) seen = seen || v == idx;
        if (seen) break;
        visited.push_back(idx);

        const auto now = std::chrono::steady_clock::now();
        if (!rt_->gate(idx, cur, now)) {
            r.ok = false;
            r.error = "[" + cur.name + "] circuit breaker open (retry after " +
                      std::to_string(cur.cbOpenMs) + "ms)";
        } else {
            switch (cur.api) {
                case ProviderApi::OpenAiChat: r = chatOpenAi(cur, req, onToken, onReasoning); break;
                case ProviderApi::Anthropic: r = chatAnthropic(cur, req, onToken, onReasoning); break;
                case ProviderApi::Gemini: r = chatGemini(cur, req, onToken, onReasoning); break;
            }
            if (r.ok) {
                rt_->onSuccess(idx, cur, r.usage.promptTokens, r.usage.completionTokens);
                lastProviderUsed = cur.name;
                r.model = model;
                return r;
            }
            rt_->onFailure(idx, cur, now);
        }
        // 失败/被熔断 → 走 fallbackTo
        if (cur.fallbackTo.empty()) break;
        auto it = byName_.find(cur.fallbackTo);
        if (it == byName_.end()) break;
        rt_->onFallback(idx);
        idx = it->second;
    }
    r.model = model;
    return r;
}

// ---- OpenAI Chat Completions（DeepSeek/OpenAI/Kimi/GLM/SiliconFlow…）----
ChatResult ProviderRouter::chatOpenAi(const ProviderConfig& p, const ChatRequest& req,
                                      const TokenCallback& onToken,
                                      const TokenCallback& onReasoning) {
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
    body["stream"] = static_cast<bool>(onToken);
    if (onToken) body["stream_options"]["include_usage"] = true;

    SimpleHttpCall call;
    call.url = p.baseURL + "/chat/completions";
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"Authorization", "Bearer " + p.apiKey}};
    call.timeoutMs = p.timeoutMs;
    call.insecureTls = p.insecure;

    if (onToken) {
        std::string content, reasoning;
        SseTranslator tr{ProviderApi::OpenAiChat, &content, &reasoning, &onToken,
                         &onReasoning};
        call.onData = [&](std::string_view sv) { return tr.feed(sv); };
        auto res = simpleHttpRequest(call);
        ChatResult r;
        if (!tr.sawAny) {
            r.error = "[" + p.name + "] " + (tr.error.empty() ? res.error : tr.error);
            return r;
        }
        r.ok = true;
        r.content = std::move(content);
        r.reasoning = std::move(reasoning);
        r.finishReason = tr.finish.empty() ? "stop" : tr.finish;
        if (tr.promptTokens >= 0) r.usage.promptTokens = tr.promptTokens;
        if (tr.completionTokens >= 0) r.usage.completionTokens = tr.completionTokens;
        return r;
    }

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
ChatResult ProviderRouter::chatAnthropic(const ProviderConfig& p, const ChatRequest& req,
                                         const TokenCallback& onToken,
                                         const TokenCallback& onReasoning) {
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
    body["stream"] = static_cast<bool>(onToken);

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
    call.insecureTls = p.insecure;

    if (onToken) {
        std::string content, reasoning;
        SseTranslator tr{ProviderApi::Anthropic, &content, &reasoning, &onToken,
                         &onReasoning};
        call.onData = [&](std::string_view sv) { return tr.feed(sv); };
        auto res = simpleHttpRequest(call);
        ChatResult r;
        if (!tr.sawAny) {
            r.error = "[" + p.name + "] " + (tr.error.empty() ? res.error : tr.error);
            return r;
        }
        r.ok = true;
        r.content = std::move(content);
        r.reasoning = std::move(reasoning);
        r.finishReason = tr.finish.empty() ? "stop" : tr.finish;
        if (tr.completionTokens >= 0) r.usage.completionTokens = tr.completionTokens;
        return r;
    }

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
ChatResult ProviderRouter::chatGemini(const ProviderConfig& p, const ChatRequest& req,
                                      const TokenCallback& onToken,
                                      const TokenCallback& onReasoning) {
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
    call.url = base + "/models/" + req.model +
               (onToken ? ":streamGenerateContent?alt=sse" : ":generateContent");
    call.body = body.dump();
    call.headers = {{"Content-Type", "application/json"},
                    {"x-goog-api-key", p.apiKey}};
    call.timeoutMs = p.timeoutMs;
    call.insecureTls = p.insecure;

    if (onToken) {
        std::string content, reasoning;
        SseTranslator tr{ProviderApi::Gemini, &content, &reasoning, &onToken,
                         &onReasoning};
        call.onData = [&](std::string_view sv) { return tr.feed(sv); };
        auto res = simpleHttpRequest(call);
        ChatResult r;
        if (!tr.sawAny) {
            r.error = "[" + p.name + "] " + (tr.error.empty() ? res.error : tr.error);
            return r;
        }
        r.ok = true;
        r.content = std::move(content);
        r.finishReason = tr.finish.empty() ? "STOP" : tr.finish;
        return r;
    }

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
    call.insecureTls = p->insecure;
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
    call.insecureTls = p.insecure;
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
