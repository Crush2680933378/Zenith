// ============================================================================
//  zenith::config::JsonConfig —— JSON 配置文件（自研解析器驱动）
//  - 嵌套访问用点路径：get("server.port", "8080")
//  - flatten() → KeyValueConfig（供宿主 C ABI config_get 使用）
//  - 数组访问：get("users.0.name")
// ============================================================================
#pragma once

#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"

namespace zenith::config {

class ZENITH_API JsonConfig {
public:
    JsonConfig() = default;

    /// 读取并解析配置文件；文件不存在/解析失败返回 Err。
    static core::Result<JsonConfig> loadFile(const std::string& path);
    static core::Result<JsonConfig> fromString(const std::string& text);

    /// 点路径取值（标量转字符串：字符串原样、数字/布尔转文本）；缺失返回 def。
    [[nodiscard]] std::string get(const std::string& dottedKey,
                                  const std::string& def = "") const;

    /// 取子对象（返回拷贝；不存在返回 Null）。
    [[nodiscard]] json::JsonValue section(const std::string& dottedKey) const;

    /// 拼平为 KeyValueConfig（"a.b.c" = "v"）。
    [[nodiscard]] core::KeyValueConfig flatten() const;

    [[nodiscard]] const json::JsonValue& root() const noexcept { return root_; }
    [[nodiscard]] bool empty() const noexcept { return root_.empty(); }

private:
    explicit JsonConfig(json::JsonValue root) : root_(std::move(root)) {}

    static void flattenInto(const json::JsonValue& node, const std::string& prefix,
                            core::KeyValueConfig& out);

    json::JsonValue root_ = json::JsonValue::object();
};

} // namespace zenith::config
