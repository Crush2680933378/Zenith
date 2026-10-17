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

} // namespace zenith::http
