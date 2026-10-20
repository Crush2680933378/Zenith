// ============================================================================
//  zenith::http 静态文件服务（M5.1）
//  staticHandler(rootDir) → Handler：挂到 catch-all 路由即得静态服务。
//  能力：Content-Type 映射、ETag/Last-Modified 协商缓存（304）、
//        Range 断点续传（单段 206 / 非法忽略回 200）、目录穿越防护。
//  性能：首版常规 send（零拷贝 TransmitFile/sendfile 通道为 M5.4 调优项）。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>

#include "zenith/http/context.hpp"

namespace zenith::http {

/// 静态文件处理器工厂。urlPrefix 为挂载前缀（如 "/assets"），
/// 请求 path 去前缀后拼接到 rootDir 下。
/// cacheSmallFiles：≤64KB 文件进程内缓存（ETag 失效），静态场景显著削磁盘读。
ZENITH_API Handler staticHandler(std::string rootDir, std::string urlPrefix,
                                 bool cacheSmallFiles = true);

/// ETag 计算（size+mtime 十六进制），独立导出便于测试
ZENITH_API std::string computeFileEtag(uint64_t size, int64_t mtimeSec);

/// Content-Type 按扩展名映射（未知返回 application/octet-stream）
ZENITH_API std::string mimeTypeFor(const std::string& path);

/// 单页应用（SPA）静态服务（M5.7）。
///
/// 行为：先按 staticHandler 语义找文件（含 ETag/304/Range）；未命中时回退
/// 返回 rootDir/index.html 并给 200——把路径裁决权交还前端路由
/// （React Router / Vue Router），刷新深链不再 404。
///
/// 教学要点（为什么需要它，而不是应用层手写回退）：
/// 1. 常见错误写法是把 catch-all 直接指向 index.html——那会让
///    /assets/app.js 这类「真实文件但路径拼错」的请求也返回 200 + HTML，
///    Content-Type 变 text/html，缓存协商全部失效，前端拿到的是垃圾字节。
/// 2. 正确顺序是「先文件后回退」：文件存在与否决定语义，只有确实不存在
///    的路径才可能是前端路由，才回 index.html。
/// 3. 挂法：`router().get("/*path", spaHandler("./web/dist"))`。
///    注意根路径 "/" 不命中 catch-all（空剩余段），需另行注册：
///    `router().get("/", spaHandler("./web/dist"))`，同一 handler 即可。
/// 4. index.html 回退建议带 Cache-Control: no-cache——它引用的
///    /assets/*.js 都是内容哈希命名，入口文件必须每次校验，否则发版后
///    旧壳引用新哈希资源会加载失败。
ZENITH_API Handler spaHandler(std::string rootDir);

} // namespace zenith::http
