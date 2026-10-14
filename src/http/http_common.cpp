#include "zenith/http/http_common.hpp"
#include "zenith/http/http_message.hpp"

namespace zenith::http {

const char* toString(HttpMethod m) noexcept {
    switch (m) {
        case HttpMethod::Get:     return "GET";
        case HttpMethod::Post:    return "POST";
        case HttpMethod::Put:     return "PUT";
        case HttpMethod::Delete:  return "DELETE";
        case HttpMethod::Patch:   return "PATCH";
        case HttpMethod::Head:    return "HEAD";
        case HttpMethod::Options: return "OPTIONS";
        default:                  return "UNKNOWN";
    }
}

HttpMethod methodFromString(const std::string& s) noexcept {
    const std::string u = core::string_util::toUpper(s);
    if (u == "GET") return HttpMethod::Get;
    if (u == "POST") return HttpMethod::Post;
    if (u == "PUT") return HttpMethod::Put;
    if (u == "DELETE") return HttpMethod::Delete;
    if (u == "PATCH") return HttpMethod::Patch;
    if (u == "HEAD") return HttpMethod::Head;
    if (u == "OPTIONS") return HttpMethod::Options;
    return HttpMethod::Unknown;
}

const char* statusText(int code) noexcept {
    switch (code) {
        case 100: return "Continue";
        case 101: return "Switching Protocols";
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 206: return "Partial Content";
        case 301: return "Moved Permanently";
        case 302: return "Found";
        case 303: return "See Other";
        case 304: return "Not Modified";
        case 307: return "Temporary Redirect";
        case 308: return "Permanent Redirect";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 408: return "Request Timeout";
        case 409: return "Conflict";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 414: return "URI Too Long";
        case 415: return "Unsupported Media Type";
        case 416: return "Range Not Satisfiable";
        case 417: return "Expectation Failed";
        case 421: return "Misdirected Request";
        case 426: return "Upgrade Required";
        case 429: return "Too Many Requests";
        case 431: return "Request Header Fields Too Large";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        case 505: return "HTTP Version Not Supported";
        default:  return "Unknown";
    }
}

namespace {
int hexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
char hexChar(int v) {
    return static_cast<char>(v < 10 ? '0' + v : 'A' + v - 10);
}
} // namespace

std::string urlDecode(const std::string& s, bool plusAsSpace) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '%' && i + 2 < s.size()) {
            const int h = hexVal(s[i + 1]);
            const int l = hexVal(s[i + 2]);
            if (h >= 0 && l >= 0) {
                out.push_back(static_cast<char>((h << 4) | l));
                i += 2;
                continue;
            }
        }
        out.push_back(c == '+' && plusAsSpace ? ' ' : c);
    }
    return out;
}

namespace {
bool isUnreserved(unsigned char c) noexcept {
    if (c >= '0' && c <= '9') return true;
    if (c >= 'a' && c <= 'z') return true;
    if (c >= 'A' && c <= 'Z') return true;
    return c == '-' || c == '_' || c == '.' || c == '~';
}
} // namespace

std::string urlEncode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (isUnreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hexChar(c >> 4));
            out.push_back(hexChar(c & 0xF));
        }
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> parseQuery(const std::string& query) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const std::string& pairStr : core::string_util::split(query, '&', false)) {
        if (pairStr.empty()) continue;
        const size_t eq = pairStr.find('=');
        if (eq == std::string::npos) {
            out.emplace_back(urlDecode(pairStr, true), "");
        } else {
            out.emplace_back(urlDecode(pairStr.substr(0, eq), true),
                             urlDecode(pairStr.substr(eq + 1), true));
        }
    }
    return out;
}

bool iequals(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const char ca = a[i] >= 'A' && a[i] <= 'Z' ? static_cast<char>(a[i] - 'A' + 'a') : a[i];
        const char cb = b[i] >= 'A' && b[i] <= 'Z' ? static_cast<char>(b[i] - 'A' + 'a') : b[i];
        if (ca != cb) return false;
    }
    return true;
}

std::string HttpResponse::serialize(bool includeBody, bool connKeepAlive) const {
    std::string out;
    out.reserve(128 + headers.size() * 32 + body.size());
    out += "HTTP/";
    out += std::to_string(httpMajor);
    out += '.';
    out += std::to_string(httpMinor);
    out += ' ';
    out += std::to_string(status);
    out += ' ';
    out += statusText(status);
    out += "\r\n";

    bool hasLen = false;
    for (const auto& [k, v] : headers.entries()) {
        if (iequals(k, "Content-Length")) hasLen = true;
        if (iequals(k, "Connection")) continue; // 由参数统一控制
        out += k;
        out += ": ";
        out += v;
        out += "\r\n";
    }
    (void)hasLen; // Content-Length 统一由下方输出（此处仅探测，供未来 304 语义）
    out += "Content-Length: ";
    out += std::to_string(includeBody ? body.size() : 0);
    out += "\r\nConnection: ";
    out += connKeepAlive ? "keep-alive" : "close";
    out += "\r\n\r\n";
    if (includeBody && !body.empty()) out += body;
    return out;
}

core::Result<Url> parseUrl(const std::string& url) {
    Url u;
    std::string rest = url;

    // scheme
    const size_t ss = rest.find("://");
    if (ss != std::string::npos) {
        u.scheme = core::string_util::toLower(rest.substr(0, ss));
        if (u.scheme != "http" && u.scheme != "https") {
            return core::Result<Url>::Err(core::Error(90, "unsupported scheme: " + u.scheme));
        }
        u.tls = u.scheme == "https";
        rest = rest.substr(ss + 3);
    }

    // 路径与查询
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
        u.pathAndQuery = rest.substr(slash);
        rest = rest.substr(0, slash);
    }
    if (u.pathAndQuery.empty()) u.pathAndQuery = "/";

    // userinfo（忽略，取 @ 后）
    const size_t at = rest.rfind('@');
    if (at != std::string::npos) rest = rest.substr(at + 1);

    // host[:port]（IPv6 字面量暂不支持 —— 后续里程碑按需）
    const size_t colon = rest.rfind(':');
    if (colon != std::string::npos) {
        u.host = rest.substr(0, colon);
        const std::string portStr = rest.substr(colon + 1);
        if (portStr.empty() ||
            portStr.find_first_not_of("0123456789") != std::string::npos) {
            return core::Result<Url>::Err(core::Error(91, "bad port in url: " + url));
        }
        u.port = static_cast<uint16_t>(std::stoi(portStr));
    } else {
        u.host = rest;
        u.port = u.tls ? 443 : 80;
    }
    if (u.host.empty()) {
        return core::Result<Url>::Err(core::Error(92, "empty host in url: " + url));
    }
    return core::Result<Url>::Ok(std::move(u));
}

} // namespace zenith::http
