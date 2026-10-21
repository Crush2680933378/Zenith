// ============================================================================
//  zenith::plugin::PluginWatcher —— 插件热重载（M8.2）
//
//  做两件事：
//    1. **`.new` 交换协议**（Windows 必需）：运行中的 DLL 不能覆盖，
//       所以换版本走「先落盘成 `<name>.dll.new`」→ watcher 检测到 →
//       卸载旧插件 → 用新文件替换原文件 → 重新加载并 start。
//    2. **直接覆盖检测**（POSIX 友好）：原文件 mtime/size 变化时重载。
//
//  用法：
//      PluginWatcher watcher(mgr, "./plugins", 1000);
//      watcher.onEvent([](const PluginWatcher::Event& e) {
//          if (!e.ok) ZLOG_ERROR("reload failed: ", e.message);
//      });
//      watcher.start();          // 后台线程
//      watcher.pollOnce();       // 或手动触发一轮（测试/CI 用）
//
//  注意：重载后**服务实例指针会失效**，消费者必须重新 queryRaw。
//        改配置也走这条路（reload 会重新 init()，插件重新读 config）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"

namespace zenith::plugin {

class PluginManager;

class ZENITH_API PluginWatcher : private core::NonCopyable {
public:
    struct Event {
        std::string file;    ///< 触发重载的文件
        std::string plugin;  ///< 插件名（失败时可能为空）
        bool ok = false;
        std::string message; ///< 失败原因
    };
    using Callback = std::function<void(const Event&)>;

    /// dir：插件目录；intervalMs：轮询间隔（0 = 只用 pollOnce）
    PluginWatcher(PluginManager& mgr, std::string dir, unsigned intervalMs = 1000);
    ~PluginWatcher();

    void start();
    void stop();

    /// 立即检查一轮：处理全部 `*.new` 与内容变化的插件文件。
    /// 返回本轮成功重载的数量。
    size_t pollOnce();

    void onEvent(Callback cb) { cb_ = std::move(cb); }
    [[nodiscard]] uint64_t reloadCount() const noexcept { return reloads_.load(); }

private:
    void loop();
    /// 交换：<name>.dll.new → <name>.dll，然后 reload
    bool swapAndReload(const std::string& newFile, Event& ev);

    PluginManager& mgr_;
    std::string dir_;
    unsigned intervalMs_;
    std::thread worker_;
    std::atomic<bool> stop_{false};
    std::atomic<uint64_t> reloads_{0};
    Callback cb_;
};

} // namespace zenith::plugin
