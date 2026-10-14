// ============================================================================
//  zenith::http 消息结构：HttpHeaders / HttpRequest / HttpResponse
//  header 名大小写不敏感（存储保留原样，查找归一小写）。
// ============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>

#include "zenith/http/http_common.hpp"

namespace zenith::http {

class HttpHeaders {
public:
    /// 追加（同名字段多次出现合法，如 Cookie/Set-Cookie）。
    void add(const std::string& name, const std::string& value) {
        entries_.emplace_back(name, value);
    }
    /// 设置（替换全部同名字段）。
    void set(const std::string& name, const std::string& value) {
        remove(name);
        entries_.emplace_back(name, value);
    }
    /// 首个匹配；不存在返回 nullptr。
    [[nodiscard]] const std::string* find(const std::string& name) const noexcept {
        for (const auto& [k, v] : entries_) {
            if (iequals(k, name)) return &v;
        }
        return nullptr;
    }
    [[nodiscard]] std::string get(const std::string& name,
                                  const std::string& def = "") const noexcept {
        const std::string* v = find(name);
        return v != nullptr ? *v : def;
    }
    [[nodiscard]] bool contains(const std::string& name) const noexcept {
        return find(name) != nullptr;
    }
    void remove(const std::string& name) {
        std::vector<Entry> kept;
        kept.reserve(entries_.size());
        for (auto& e : entries_) {
            if (!iequals(e.first, name)) kept.push_back(std::move(e));
        }
        entries_ = std::move(kept);
    }
    [[nodiscard]] bool empty() const noexcept { return entries_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return entries_.size(); }

    using Entry = std::pair<std::string, std::string>;
    [[nodiscard]] const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Get;
    int httpMajor = 1;
    int httpMinor = 1;
    std::string path;                 // 已 percent 解码
    std::string rawQuery;             // 原始 query（无 '?'）
    std::unordered_map<std::string, std::string> query; // 已解码
    HttpHeaders headers;
    std::string body;
    std::string peer;                 // "ip:port"

    [[nodiscard]] bool keepAlive() const noexcept {
        const std::string conn = core::string_util::toLower(headers.get("Connection"));
        if (httpMajor == 1 && httpMinor == 1) return conn != "close";
        if (httpMajor == 1 && httpMinor == 0) return conn == "keep-alive";
        return false;
    }
    [[nodiscard]] size_t contentLength() const noexcept {
        const std::string v = headers.get("Content-Length");
        if (v.empty()) return 0;
        size_t n = 0;
        for (char c : v) {
            if (c < '0' || c > '9') return 0;
            n = n * 10 + static_cast<size_t>(c - '0');
            if (n > (1ull << 40)) return n; // 防溢出，交上层按上限拒绝
        }
        return n;
    }
    [[nodiscard]] std::string header(const std::string& name,
                                     const std::string& def = "") const noexcept {
        return headers.get(name, def);
    }
};

struct HttpResponse {
    int httpMajor = 1;
    int httpMinor = 1;
    int status = 200;
    HttpHeaders headers;
    std::string body;

    [[nodiscard]] bool keepAlive() const noexcept {
        const std::string conn = core::string_util::toLower(headers.get("Connection"));
        if (httpMajor == 1 && httpMinor == 1) return conn != "close";
        return conn == "keep-alive";
    }

    /// 序列化（含 Content-Length）。includeBody=false 用于 HEAD 应答。
    [[nodiscard]] std::string serialize(bool includeBody, bool connKeepAlive) const;
};

} // namespace zenith::http
