// ============================================================================
//  zenith::http::compress —— 响应压缩（M7.4 落地形态，zlib 后端，可选编入）
//  - 自研栈不引入第三方压缩器，改由 C ABI 的 zlib 承担 codec（README 依赖原则）
//  - zlib 未探测到时全部降级为 identity（applyGzip 返回 false），链接不受影响
// ============================================================================
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"
#include "zenith/http/context.hpp"

namespace zenith::http {

/// gzip 压缩（RFC 1952）。后端缺失或失败返回 nullopt。
ZENITH_API std::optional<std::string> gzipCompress(std::string_view data, int level = 6);
/// gzip 解压（交叉核对/客户端解码用）。后端缺失或失败返回 nullopt。
ZENITH_API std::optional<std::string> gzipDecompress(std::string_view data);

/// 按 Accept-Encoding 与体积阈值决定是否压缩；成功压缩返回 true。
/// 跳过条件：已 chunked / 204·304 / body 小于 minBytes / 已带 Content-Encoding /
/// 客户端不接受 gzip / 压缩后反而变大。
ZENITH_API bool applyGzip(const HttpRequest& req, HttpResponse& resp, size_t minBytes = 1024);

/// 中间件形态：全局启用响应压缩（handler 返回后执行）。
ZENITH_API Middleware gzipMiddleware(size_t minBytes = 1024);

} // namespace zenith::http
