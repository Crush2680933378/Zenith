// ============================================================================
//  zenith::http::multipart —— form-data 解析（M5.1）
//  解析 multipart/form-data 主体：普通字段 + 文件（含 filename/content-type）。
//  主体已由 HttpParser 完整缓冲（受 maxBodyBytes 上限保护），此处纯内存扫描。
// ============================================================================
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::http::multipart {

struct FormPart {
    std::string name;         // 字段名
    std::string filename;     // 文件名（普通字段为空）
    std::string contentType;  // 文件 Content-Type（可空）
    std::string data;         // 内容（二进制安全）

    [[nodiscard]] bool isFile() const noexcept { return !filename.empty(); }
};

/// 从 Content-Type 头提取 boundary 参数；缺失返回 Err。
ZENITH_API core::Result<std::string> extractBoundary(const std::string& contentType);

/// 解析主体。boundary 为 extractBoundary 的结果（不含 "--" 前缀）。
/// 容错策略：结构损坏立即返回 Err；空 parts 合法（返回空向量）。
ZENITH_API core::Result<std::vector<FormPart>> parse(const std::string& body,
                                                     const std::string& boundary);

/// 解析 Content-Disposition 参数值（已去引号、百分号解码不做——multipart 不编码）
ZENITH_API std::optional<std::string> dispositionParam(const std::string& line,
                                                        const std::string& key);

} // namespace zenith::http::multipart
