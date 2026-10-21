#include "upstream_http.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using UpSocket = SOCKET;
  #define UP_INVALID INVALID_SOCKET
#else
  #include <arpa/inet.h>
  #include <cerrno>
  #include <netdb.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>
  using UpSocket = int;
  #define UP_INVALID (-1)
#endif

namespace zenith::ai::detail {

namespace {

#if defined(_WIN32)
struct WsaGuard {
    WsaGuard() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
void ensureWsa() {
    static WsaGuard guard;
    (void)guard;
}
#else
void ensureWsa() {}
#endif

void closeSocket(UpSocket s) {
#if defined(_WIN32)
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void setRecvTimeout(UpSocket s, int ms) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(ms);
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/// http://host[:port]/path → (host, port, path)
bool parseUrl(const std::string& url, std::string& host, uint16_t& port, std::string& path) {
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    const std::string rest = url.substr(prefix.size());
    const size_t slash = rest.find('/');
    std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t colon = authority.rfind(':');
    if (colon != std::string::npos) {
        host = authority.substr(0, colon);
        const int p = std::atoi(authority.c_str() + colon + 1);
        if (p <= 0 || p > 65535) return false;
        port = static_cast<uint16_t>(p);
    } else {
        host = authority;
        port = 80;
    }
    return !host.empty();
}

UpSocket connectTo(const std::string& host, uint16_t port, std::string& error) {
    ensureWsa();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (::getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || res == nullptr) {
        error = "resolve failed: " + host;
        return UP_INVALID;
    }
    UpSocket s = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (s == UP_INVALID) {
        ::freeaddrinfo(res);
        error = "socket() failed";
        return UP_INVALID;
    }
    if (::connect(s, res->ai_addr, static_cast<int>(res->ai_addrlen)) != 0) {
        error = "connect failed: " + host + ":" + portStr;
        closeSocket(s);
        ::freeaddrinfo(res);
        return UP_INVALID;
    }
    ::freeaddrinfo(res);
    const int one = 1;
    ::setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&one), sizeof(one));
    return s;
}

bool sendAll(UpSocket s, const std::string& buf) {
    size_t sent = 0;
    while (sent < buf.size()) {
        const int n = ::send(s, buf.data() + sent, static_cast<int>(buf.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string lower(std::string s) {
    for (auto& ch : s) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return s.substr(b, e - b);
}

} // namespace

bool httpRequest(const UpstreamCall& call, const UpstreamSink& onData, UpstreamResult& out) {
    std::string host;
    uint16_t port = 80;
    std::string path;
    if (!parseUrl(call.url, host, port, path)) {
        out.error = "unsupported url (only http://): " + call.url;
        return false;
    }

    UpSocket s = connectTo(host, port, out.error);
    if (s == UP_INVALID) return false;
    setRecvTimeout(s, call.timeoutMs);

    std::string req;
    req.reserve(256 + call.body.size());
    req += call.method;
    req += ' ';
    req += path;
    req += " HTTP/1.1\r\nHost: ";
    req += host;
    if (port != 80) {
        req += ':';
        req += std::to_string(port);
    }
    req += "\r\nConnection: close\r\n";
    bool hasContentType = false;
    for (const auto& [k, v] : call.headers) {
        if (lower(k) == "content-type") hasContentType = true;
        req += k;
        req += ": ";
        req += v;
        req += "\r\n";
    }
    if (!call.body.empty() && !hasContentType) req += "Content-Type: application/json\r\n";
    req += "Content-Length: ";
    req += std::to_string(call.body.size());
    req += "\r\n\r\n";
    req += call.body;

    if (!sendAll(s, req)) {
        out.error = "send failed";
        closeSocket(s);
        return false;
    }

    // ---- 收头 ----
    std::string buf;
    char tmp[8192];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) {
            out.error = "connection closed before headers";
            closeSocket(s);
            return false;
        }
        buf.append(tmp, static_cast<size_t>(n));
        headerEnd = buf.find("\r\n\r\n");
        if (buf.size() > (1u << 20)) {
            out.error = "response headers too large";
            closeSocket(s);
            return false;
        }
    }

    const std::string head = buf.substr(0, headerEnd);
    std::string rest = buf.substr(headerEnd + 4);

    // 状态行
    const size_t sp1 = head.find(' ');
    if (sp1 == std::string::npos) {
        out.error = "malformed status line";
        closeSocket(s);
        return false;
    }
    out.status = std::atoi(head.c_str() + sp1 + 1);
    out.transportOk = true;

    // 头
    std::map<std::string, std::string> headers;
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos + 2 < head.size()) {
        const size_t lineEnd = head.find("\r\n", pos + 2);
        const std::string line = head.substr(pos + 2, (lineEnd == std::string::npos ? head.size()
                                                                                  : lineEnd) -
                                                          (pos + 2));
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            headers[lower(trim(line.substr(0, colon)))] = trim(line.substr(colon + 1));
        }
        if (lineEnd == std::string::npos) break;
        pos = lineEnd;
    }

    const auto chunky = headers.find("transfer-encoding");
    const bool chunked = chunky != headers.end() && chunky->second.find("chunked") != std::string::npos;
    long long remaining = -1;
    if (const auto it = headers.find("content-length"); it != headers.end()) {
        remaining = std::atoll(it->second.c_str());
    }

    auto deliver = [&](std::string_view chunk) -> bool {
        if (chunk.empty()) return true;
        if (onData) return onData(chunk);
        out.body.append(chunk.data(), chunk.size());
        return true;
    };

    // ---- 正文：非分块 ----
    if (!chunked) {
        if (!deliver(rest)) {
            closeSocket(s);
            return true;
        }
        if (remaining >= 0) {
            remaining -= static_cast<long long>(std::min<size_t>(rest.size(), rest.size()));
            while (remaining > 0) {
                const int n = ::recv(s, tmp, static_cast<int>(std::min<size_t>(sizeof(tmp),
                                                                              static_cast<size_t>(remaining))),
                                     0);
                if (n <= 0) break;
                if (!deliver(std::string_view(tmp, static_cast<size_t>(n)))) {
                    closeSocket(s);
                    return true;
                }
                remaining -= n;
            }
        } else { // 读到断开
            for (;;) {
                const int n = ::recv(s, tmp, sizeof(tmp), 0);
                if (n <= 0) break;
                if (!deliver(std::string_view(tmp, static_cast<size_t>(n)))) break;
            }
        }
        closeSocket(s);
        return true;
    }

    // ---- 正文：chunked ----
    std::string pending = std::move(rest);
    enum class St { Size, Data, Trail } st = St::Size;
    size_t need = 0;
    bool aborted = false;

    auto fill = [&]() -> bool {
        const int n = ::recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        pending.append(tmp, static_cast<size_t>(n));
        return true;
    };

    for (;;) {
        if (st == St::Size) {
            const size_t eol = pending.find("\r\n");
            if (eol == std::string::npos) {
                if (!fill()) break;
                continue;
            }
            const std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 2);
            need = static_cast<size_t>(std::strtoull(line.c_str(), nullptr, 16));
            if (need == 0) {
                st = St::Trail;
                continue;
            }
            st = St::Data;
            continue;
        }
        if (st == St::Data) {
            if (pending.size() < need + 2) {
                if (!fill()) break;
                continue;
            }
            const std::string_view chunk(pending.data(), need);
            if (!deliver(chunk)) {
                aborted = true;
                break;
            }
            pending.erase(0, need + 2); // 数据 + CRLF
            st = St::Size;
            continue;
        }
        if (st == St::Trail) { // 尾随头：空行即结束
            const size_t eol = pending.find("\r\n");
            if (eol == std::string::npos) {
                if (!fill()) break;
                continue;
            }
            if (eol == 0) break;
            pending.erase(0, eol + 2);
            continue;
        }
    }
    closeSocket(s);
    if (!aborted && out.error.empty()) return true;
    return true;
}

} // namespace zenith::ai::detail
