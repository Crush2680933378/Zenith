#include "zenith/http/compress.hpp"

#if defined(ZENITH_HAS_ZLIB)
#include <zlib.h>
#endif

#include <cstring>

namespace zenith::http {

namespace {

/// 简易 Accept-Encoding 协商：支持 "gzip;q=0" 明确拒绝
bool acceptsGzip(const std::string& acceptEncoding) {
    const std::string s = core::string_util::toLower(acceptEncoding);
    if (s.empty()) return false;
    size_t pos = 0;
    while (pos < s.size()) {
        size_t end = s.find(',', pos);
        if (end == std::string::npos) end = s.size();
        std::string item = core::string_util::trim(s.substr(pos, end - pos));
        pos = end + 1;
        const size_t semi = item.find(';');
        const std::string token = semi == std::string::npos ? item : item.substr(0, semi);
        const std::string params = semi == std::string::npos ? "" : item.substr(semi);
        if (token != "gzip" && token != "*") continue;
        if (params.find("q=0") != std::string::npos && params.find("q=0.") == std::string::npos) {
            continue; // 明确拒绝
        }
        return true;
    }
    return false;
}

} // namespace

#if defined(ZENITH_HAS_ZLIB)

std::optional<std::string> gzipCompress(std::string_view data, int level) {
    if (level < 1 || level > 9) level = 6;
    z_stream zs{};
    // 15 + 16：deflate 窗口 32KB + gzip 头部封装（RFC 1952）
    if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::nullopt;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    out.resize(deflateBound(&zs, static_cast<uLong>(data.size())) + 32);
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = static_cast<uInt>(out.size());

    const int rc = deflate(&zs, Z_FINISH);
    if (rc != Z_STREAM_END) {
        deflateEnd(&zs);
        return std::nullopt;
    }
    out.resize(zs.total_out);
    deflateEnd(&zs);
    return out;
}

std::optional<std::string> gzipDecompress(std::string_view data) {
    z_stream zs{};
    if (inflateInit2(&zs, 15 + 16) != Z_OK) return std::nullopt;
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    out.resize(data.size() * 4 + 64);
    for (;;) {
        zs.next_out = reinterpret_cast<Bytef*>(out.data() + zs.total_out);
        zs.avail_out = static_cast<uInt>(out.size() - zs.total_out);
        const int rc = inflate(&zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) {
            out.resize(zs.total_out);
            inflateEnd(&zs);
            return out;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) {
            inflateEnd(&zs);
            return std::nullopt;
        }
        if (zs.avail_out == 0) out.resize(out.size() * 2);
        if (zs.avail_in == 0 && zs.avail_out != 0) { // 数据耗尽但未收尾
            inflateEnd(&zs);
            return std::nullopt;
        }
    }
}

#else // !ZENITH_HAS_ZLIB —— 无 zlib 时整体降级

std::optional<std::string> gzipCompress(std::string_view, int) { return std::nullopt; }
std::optional<std::string> gzipDecompress(std::string_view) { return std::nullopt; }

#endif // ZENITH_HAS_ZLIB

bool applyGzip(const HttpRequest& req, HttpResponse& resp, size_t minBytes) {
    if (resp.chunked) return false;                      // 流式响应不做整包压缩
    if (resp.status == 204 || resp.status == 304) return false;
    if (resp.headers.contains("Content-Encoding")) return false;
    if (resp.body.size() < minBytes) return false;
    if (req.method == HttpMethod::Head) return false;
    if (!acceptsGzip(req.headers.get("Accept-Encoding"))) return false;

    auto packed = gzipCompress(resp.body, 6);
    if (!packed.has_value() || packed->size() >= resp.body.size()) return false;

    resp.body = std::move(*packed);
    resp.headers.set("Content-Encoding", "gzip");
    resp.headers.set("Vary", "Accept-Encoding");
    resp.headers.remove("Content-Length"); // 由 serialize 按新长度重算
    return true;
}

Middleware gzipMiddleware(size_t minBytes) {
    return [minBytes](Context& c) {
        c.next();
        applyGzip(c.request(), c.response(), minBytes);
    };
}

} // namespace zenith::http
