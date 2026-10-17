#include "zenith/http/static_files.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <fstream>

namespace zenith::http {

namespace fs = std::filesystem;

std::string computeFileEtag(uint64_t size, int64_t mtimeSec) {
    char buf[48];
    std::snprintf(buf, sizeof(buf), "\"%llx-%llx\"",
                  static_cast<unsigned long long>(mtimeSec),
                  static_cast<unsigned long long>(size));
    return buf;
}

std::string mimeTypeFor(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos) return "application/octet-stream";
    const size_t q = path.find_first_of("?#", dot);
    std::string ext = path.substr(dot + 1, q == std::string::npos ? std::string::npos
                                                                   : q - dot - 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "html" || ext == "htm") return "text/html; charset=utf-8";
    if (ext == "css") return "text/css; charset=utf-8";
    if (ext == "js" || ext == "mjs") return "application/javascript; charset=utf-8";
    if (ext == "json") return "application/json; charset=utf-8";
    if (ext == "txt" || ext == "md") return "text/plain; charset=utf-8";
    if (ext == "xml") return "application/xml; charset=utf-8";
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "svg") return "image/svg+xml";
    if (ext == "webp") return "image/webp";
    if (ext == "ico") return "image/x-icon";
    if (ext == "woff") return "font/woff";
    if (ext == "woff2") return "font/woff2";
    if (ext == "ttf") return "font/ttf";
    if (ext == "mp4") return "video/mp4";
    if (ext == "webm") return "video/webm";
    if (ext == "mp3") return "audio/mpeg";
    if (ext == "wav") return "audio/wav";
    if (ext == "pdf") return "application/pdf";
    if (ext == "zip") return "application/zip";
    if (ext == "wasm") return "application/wasm";
    return "application/octet-stream";
}

namespace {
/// url 路径（已解码）→ 安全文件路径；越界返回空
std::string safeJoin(const fs::path& root, const std::string& rel) {
    // 分段过滤：拒绝 ..、绝对段、盘符痕迹
    if (!rel.empty() && (rel[0] == '/' || rel[0] == '\\')) return {};
    for (const std::string& seg : core::string_util::split(rel, '/')) {
        if (seg == ".." || seg == "." || seg.empty()) return {};
        if (seg.find(':') != std::string::npos || seg.find('\\') != std::string::npos) return {};
    }
    std::error_code ec;
    const fs::path full = fs::weakly_canonical(root / rel, ec);
    if (ec) return {};
    const fs::path rootCan = fs::weakly_canonical(root, ec);
    if (ec) return {};
    // 必须仍在 root 之下
    auto f = full.begin();
    auto r = rootCan.begin();
    for (; r != rootCan.end(); ++r, ++f) {
        if (f == full.end() || *f != *r) return {};
    }
    return full.string();
}

/// 解析 Range 头（bytes=a-b / bytes=a- / bytes=-suffix）；非法返回 false
bool parseRange(const std::string& v, uint64_t size, uint64_t& start, uint64_t& end) {
    if (v.compare(0, 6, "bytes=") != 0) return false;
    const std::string spec = v.substr(6);
    if (spec.find(',') != std::string::npos) return false; // 多段：忽略回全量
    const size_t dash = spec.find('-');
    if (dash == std::string::npos) return false;
    const std::string a = spec.substr(0, dash);
    const std::string b = spec.substr(dash + 1);
    if (a.empty()) {
        // 后缀形式
        if (b.empty()) return false;
        uint64_t n = 0;
        for (char c : b) {
            if (c < '0' || c > '9') return false;
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        if (n == 0 || n > size) n = size;
        start = size - n;
        end = size - 1;
        return true;
    }
    uint64_t s = 0;
    for (char c : a) {
        if (c < '0' || c > '9') return false;
        s = s * 10 + static_cast<uint64_t>(c - '0');
    }
    if (s >= size) return false;
    if (b.empty()) {
        start = s;
        end = size - 1;
        return true;
    }
    uint64_t e = 0;
    for (char c : b) {
        if (c < '0' || c > '9') return false;
        e = e * 10 + static_cast<uint64_t>(c - '0');
    }
    if (e < s) return false;
    start = s;
    end = std::min(e, size - 1);
    return true;
}
} // namespace

namespace {
// 小文件缓存（ETag 键；上限 256 条防膨胀）。M6.2：削静态热路径磁盘读。
constexpr uint64_t kCacheMaxFile = 64 * 1024;
constexpr size_t kCacheMaxEntries = 256;
struct CacheEntry {
    std::string etag;
    std::string body;
    int64_t mtime;
};
std::mutex gCacheMtx;
std::map<std::string, CacheEntry> gCache;

bool cacheGet(const std::string& path, const std::string& etag, std::string& outBody) {
    std::lock_guard<std::mutex> lk(gCacheMtx);
    auto it = gCache.find(path);
    if (it == gCache.end()) return false;
    if (it->second.etag != etag) {
        gCache.erase(it); // 文件已变 —— 失效
        return false;
    }
    outBody = it->second.body;
    return true;
}

void cachePut(const std::string& path, const std::string& etag, const std::string& body,
              int64_t mtime) {
    std::lock_guard<std::mutex> lk(gCacheMtx);
    if (gCache.size() >= kCacheMaxEntries) {
        // 简单逐出：清最旧 mtime 的一半
        std::vector<const std::string*> doomed;
        for (const auto& [p, e] : gCache) doomed.push_back(&p);
        std::sort(doomed.begin(), doomed.end(), [&](const std::string* a, const std::string* b) {
            return gCache[*a].mtime < gCache[*b].mtime;
        });
        for (size_t i = 0; i < doomed.size() / 2; ++i) gCache.erase(*doomed[i]);
    }
    gCache[path] = CacheEntry{etag, body, mtime};
}
} // namespace

Handler staticHandler(std::string rootDir, std::string urlPrefix, bool cacheSmallFiles) {
    return [root = std::move(rootDir), prefix = std::move(urlPrefix),
            cache = cacheSmallFiles](Context& c) {
        // 去前缀得相对路径
        std::string rel = c.path();
        if (rel.compare(0, prefix.size(), prefix) == 0) {
            rel = rel.substr(prefix.size());
        }
        while (!rel.empty() && rel[0] == '/') rel.erase(0, 1);

        const std::string full = safeJoin(fs::path(root), rel);
        if (full.empty()) {
            c.status(403).text(403, "403 forbidden");
            return;
        }

        std::error_code ec;
        fs::path fp(full);
        if (!fs::is_regular_file(fp, ec)) {
            c.status(404).text(404, "404 not found");
            return;
        }
        const auto size = static_cast<uint64_t>(fs::file_size(fp, ec));
        const int64_t mtime = static_cast<int64_t>(
            fs::last_write_time(fp, ec).time_since_epoch().count());

        const std::string etag = computeFileEtag(size, mtime);

        // 协商缓存
        const std::string inm = c.header("If-None-Match");
        if (!inm.empty() && inm == etag) {
            c.status(304);
            c.setHeader("ETag", etag);
            return;
        }

        std::ifstream in(full, std::ios::binary);
        if (!in) {
            c.status(404).text(404, "404 not found");
            return;
        }

        c.setHeader("ETag", etag);
        c.setHeader("Accept-Ranges", "bytes");
        c.setHeader("Content-Type", mimeTypeFor(full));

        // Range
        const std::string range = c.header("Range");

        // 小文件缓存快路径：全量 200 且 ETag 未变 → 免磁盘读
        if (cache && range.empty() && size > 0 && size <= kCacheMaxFile) {
            std::string cached;
            if (cacheGet(full, etag, cached)) {
                c.status(200);
                c.response().body = std::move(cached);
                return;
            }
            std::string body(static_cast<size_t>(size), ' ');
            in.read(body.data(), static_cast<std::streamsize>(size));
            cachePut(full, etag, body, mtime);
            c.status(200);
            c.response().body = std::move(body);
            return;
        }

        uint64_t start = 0, end = size == 0 ? 0 : size - 1;
        bool partial = false;
        if (!range.empty() && size > 0 && parseRange(range, size, start, end)) {
            partial = true;
            c.status(206);
            c.setHeader("Content-Range",
                        "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
                            std::to_string(size));
        } else {
            c.status(200);
        }

        const uint64_t len = size == 0 ? 0 : (end - start + 1);
        in.seekg(static_cast<std::streamoff>(start));
        std::string body;
        body.resize(static_cast<size_t>(len));
        if (len > 0) {
            in.read(body.data(), static_cast<std::streamsize>(len));
        }
        c.response().body = std::move(body);
    };
}

} // namespace zenith::http
