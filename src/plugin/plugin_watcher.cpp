#include "zenith/plugin/plugin_watcher.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#include "zenith/log/logger.hpp"
#include "zenith/plugin/plugin_manager.hpp"

namespace zenith::plugin {

namespace fs = std::filesystem;

PluginWatcher::PluginWatcher(PluginManager& mgr, std::string dir, unsigned intervalMs)
    : mgr_(mgr), dir_(std::move(dir)), intervalMs_(intervalMs) {}

PluginWatcher::~PluginWatcher() { stop(); }

void PluginWatcher::start() {
    if (worker_.joinable()) return;
    stop_.store(false);
    worker_ = std::thread([this] { loop(); });
}

void PluginWatcher::stop() {
    stop_.store(true);
    if (worker_.joinable()) worker_.join();
}

void PluginWatcher::loop() {
    while (!stop_.load()) {
        pollOnce();
        // 分片睡眠：stop() 最多等一个分片
        const unsigned total = intervalMs_ == 0 ? 200 : intervalMs_;
        for (unsigned slept = 0; slept < total && !stop_.load(); slept += 50) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

bool PluginWatcher::swapAndReload(const std::string& newFile, Event& ev) {
    ev.file = newFile;
    // foo.dll.new → foo.dll
    std::string target = newFile;
    if (target.size() > 4 && target.compare(target.size() - 4, 4, ".new") == 0) {
        target.resize(target.size() - 4);
    } else {
        ev.message = "not a .new file: " + newFile;
        return false;
    }

    // 1) 先卸载旧插件（Windows 上不卸载就无法覆盖）
    mgr_.unload(target);
    if (PluginInfo* remaining = mgr_.find(target); remaining != nullptr) {
        ev.message = "old plugin still loaded: " + remaining->name;
        return false;
    }

    // 2) 替换：旧文件先挪开（Windows 下即便已 dlclose 也可能被占用），再改名
    std::error_code ec;
    fs::remove(target + ".old", ec);
    if (fs::exists(target, ec)) fs::rename(target, target + ".old", ec);
    if (ec) {
        ev.message = "cannot move old file aside: " + ec.message();
        return false;
    }
    fs::rename(newFile, target, ec);
    if (ec) {
        ev.message = "cannot install new file: " + ec.message();
        // 尽力回滚
        std::error_code ec2;
        fs::rename(target + ".old", target, ec2);
        return false;
    }
    fs::remove(target + ".old", ec); // 尽力清理

    // 3) 重新加载并 start
    auto r = mgr_.reload(target);
    if (!r) {
        ev.ok = false;
        ev.message = r.error().message;
        ZLOG_ERROR("plugin reload failed: ", target, " -> ", ev.message);
        return false;
    }
    ev.ok = true;
    ev.plugin = r.value()->name;
    ev.message = "reloaded";
    reloads_.fetch_add(1);
    ZLOG_INFO("plugin hot-reloaded from ", newFile, " -> ", ev.plugin);
    return true;
}

size_t PluginWatcher::pollOnce() {
    std::error_code ec;
    if (!fs::is_directory(dir_, ec)) return 0;

    // 先收集：就地改名会干扰迭代器
    std::vector<std::string> pending;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string p = entry.path().string();
        if (p.size() > 4 && p.compare(p.size() - 4, 4, ".new") == 0) pending.push_back(p);
    }

    size_t ok = 0;
    for (const std::string& f : pending) {
        Event ev;
        if (swapAndReload(f, ev)) {
            ++ok;
        } else if (!ev.message.empty()) {
            ZLOG_WARN("plugin swap skipped: ", ev.file, " (", ev.message, ")");
        }
        if (cb_) cb_(ev);
    }
    return ok;
}

} // namespace zenith::plugin
