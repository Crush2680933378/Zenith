// ============================================================================
//  zenith::http 公共类型：方法、状态码、URL 编解码、大小写不敏感比较
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/string.hpp"
#include "zenith/json/json.hpp"

namespace zenith::http {

/// 便捷别名：http 层内直接写 http::json::JsonValue。
namespace json = ::zenith::json;

enum class HttpMethod : uint8_t {
    Get = 0,
    Post,
    Put,
    Delete,
    Patch,
    Head,
    Options,
    Unknown,
};

ZENITH_API const char* toString(HttpMethod m) noexcept;
ZENITH_API HttpMethod methodFromString(const std::string& s) noexcept;

/// 标准状态码 → 原因短语（未知返回 "Unknown"）。
ZENITH_API const char* statusText(int code) noexcept;

/// percent 解码；plusAsSpace=true 时把 '+' 解为空格（query 语义）。
ZENITH_API std::string urlDecode(const std::string& s, bool plusAsSpace = false);

/// percent 编码（query 组件安全集：字母数字与 -_.~ 直通）。
ZENITH_API std::string urlEncode(const std::string& s);

/// 解析 "a=1&b=2" 形式（值已 urlDecode）。
ZENITH_API std::vector<std::pair<std::string, std::string>>
parseQuery(const std::string& query);

/// 大小写不敏感相等（header 名比较用）。
ZENITH_API bool iequals(const std::string& a, const std::string& b) noexcept;

/// URL 解析（M4.2）：scheme://host[:port]/path[?query]
/// scheme 缺省 http；https → tls=true；端口缺省 80/443。
struct Url {
    std::string scheme = "http";
    std::string host;
    uint16_t port = 80;
    std::string pathAndQuery = "/"; // 以 / 开头，可含 ?query
    bool tls = false;
};
ZENITH_API core::Result<Url> parseUrl(const std::string& url);

/// 分块编码单块（M9.1 流式响应/SSE）：
/// "hex(长度)\r\n" + data + "\r\n"；data 为空返回空串（终止块另行写出 0\r\n\r\n）。
ZENITH_API std::string chunkEncode(std::string_view data);

} // namespace zenith::http
