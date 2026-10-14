// ============================================================================
//  KeyValueConfig —— M1 级配置源：扁平 key=value 存储
//  - 供宿主 ABI config_get 使用；M2 将升级为 JSON 配置（自研解析器），
//    本类保留为 JSON 配置的扁平视图。
//  - 线程安全：读写均加锁。key 不区分大小写（统一小写存储）。
// ============================================================================
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::core {

class ZENITH_API KeyValueConfig {
public:
    KeyValueConfig() = default;

    // 含 mutex，需显式移动构造（拷贝保持删除）
    KeyValueConfig(KeyValueConfig&& other) noexcept {
        std::lock_guard<std::mutex> lk(other.mtx_);
        map_ = std::move(other.map_);
    }
    KeyValueConfig& operator=(KeyValueConfig&& other) noexcept {
        if (this != &other) {
            std::unique_lock<std::mutex> lk1(mtx_, std::defer_lock);
            std::unique_lock<std::mutex> lk2(other.mtx_, std::defer_lock);
            std::lock(lk1, lk2);
            map_ = std::move(other.map_);
        }
        return *this;
    }

    void set(const std::string& key, const std::string& value);
    [[nodiscard]] std::string get(const std::string& key,
                                  const std::string& defaultValue = "") const;
    [[nodiscard]] bool tryGet(const std::string& key, std::string& outValue) const;
    [[nodiscard]] bool contains(const std::string& key) const;
    [[nodiscard]] size_t size() const;
    void remove(const std::string& key);
    void clear();

    /// 解析 "key=value" 文本（# 与 ; 为行注释，允许空白）。解析错误不抛出，
    /// 逐行容忍并继续；返回解析成功的条目数。
    size_t parseFromString(const std::string& text);

    /// 读取 UTF-8 文本文件并解析。文件不存在返回 Err。
    Result<void> loadFromFile(const std::string& path);

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> entries() const;

private:
    mutable std::mutex mtx_;
    std::map<std::string, std::string> map_;
};

} // namespace zenith::core