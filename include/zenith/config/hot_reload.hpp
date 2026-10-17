// ============================================================================
//  zenith::config::HotReloader —— 配置热重载（M5.6）
//  - 文件变更检测：内容哈希轮询（跨平台可靠——实测 NTFS 下 last_write_time
//    对同长覆写不敏感，仅重建可见；配置文件小，全量读取开销可忽略）
//  - 手动触发：reload()（POSIX 下另接 SIGHUP —— 见 installSignalHandler 组合）
//  - 原子快照：get() 返回 shared_ptr<const Config>（读写无锁）
//  - 失败安全：解析失败保留旧快照，onError 回调报告
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <thread>

#include "zenith/config/json_config.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::config {

class ZENITH_API HotReloader {
public:
    using ConfigPtr = std::shared_ptr<const JsonConfig>;
    using ChangeCallback = std::function<void(ConfigPtr newCfg)>;
    using ErrorCallback = std::function<void(const std::string& message)>;

    /// path：JSON 配置文件；intervalMs：轮询间隔（0 = 仅手动）
    HotReloader(std::string path, unsigned intervalMs = 500);
    ~HotReloader();

    HotReloader(const HotReloader&) = delete;
    HotReloader& operator=(const HotReloader&) = delete;

    /// 启动监视线程（幂等）。首次装载失败也会启动（后续变更可恢复）。
    void start();

    /// 停止（析构自动调用；幂等）
    void stop();

    /// 手动触发重载（线程安全）。成功返回 true。
    bool reload();

    /// 当前快照（无锁读取；启动前为空对象）
    [[nodiscard]] ConfigPtr get() const noexcept { return std::atomic_load(&current_); }

    /// 变更回调（成功装载新配置后触发，监视线程上下文）
    void onChange(ChangeCallback cb) { onChange_ = std::move(cb); }
    /// 失败回调（IO/解析错误；旧快照保留）
    void onError(ErrorCallback cb) { onError_ = std::move(cb); }

    /// 首次装载是否成功
    [[nodiscard]] bool loaded() const noexcept { return loaded_.load(); }
    /// 成功重载次数
    [[nodiscard]] uint64_t reloadCount() const noexcept { return reloadCount_.load(); }

private:
    void monitorLoop();
    bool reloadLocked();
    [[nodiscard]] int64_t fileMtime() const;
    /// 读文件全文（失败返回 nullopt）
    [[nodiscard]] std::optional<std::string> readFile() const;
    [[nodiscard]] static uint64_t fnv1a(const std::string& s);

    std::string path_;
    unsigned intervalMs_;
    std::thread worker_;
    std::mutex wakeMtx_;
    std::condition_variable wakeCv_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> loaded_{false};
    std::atomic<uint64_t> reloadCount_{0};
    int64_t lastMtime_ = 0;
    uint64_t lastHash_ = 0;
    bool hasBaseline_ = false;

    std::shared_ptr<const JsonConfig> current_;
    ChangeCallback onChange_;
    ErrorCallback onError_;
};

} // namespace zenith::config
