// ============================================================================
//  PluginManager —— 插件发现、加载、生命周期与安全卸载
//
//  加载目录搜索顺序：
//    1. loadDirectory(dir) 的显式目录
//    2. 环境变量 ZENITH_PLUGIN_PATH（Windows 用 ';'、POSIX 用 ':' 分隔）
//    3. 可执行文件同级 plugins/ 目录（由宿主自行调用）
//
//  卸载安全序（ unloadAll 逐插件逆序执行）：
//    stop → shutdown → registry.removeByOwner(token) → dlclose
//  —— 先清理注册表再卸库，保证 destroy 回调执行时代码段仍在内存。
// ============================================================================
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "zenith/abi/zenith_abi.h"
#include "zenith/core/export.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"
#include "zenith/plugin/dynlib.hpp"
#include "zenith/plugin/host_api.hpp"
#include "zenith/plugin/service_registry.hpp"

namespace zenith::plugin {

enum class PluginState {
    Unloaded,
    Loaded,      // dlopen 成功、desc 已校验
    Initialized, // init(host) 成功
    Started,     // start() 成功
    Stopped,     // stop() 已调用
};

struct PluginInfo {
    std::string path;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    PluginState state = PluginState::Unloaded;
    zenith_plugin_desc desc{};
    zenith_host_api hostApi{}; // 注入给插件的宿主能力表（生命周期随本对象）
    std::unique_ptr<DynLib> lib;
};

class ZENITH_API PluginManager : private core::NonCopyable {
public:
    PluginManager(ServiceRegistry& registry,
                  concurrency::ThreadPool* pool = nullptr,
                  core::KeyValueConfig* config = nullptr);
    ~PluginManager();

    /// 加载单个插件文件（load + desc 校验 + init）。
    core::Result<PluginInfo*> load(const std::string& path);

    /// 扫描目录加载全部插件文件（按文件名排序，保证确定性），
    /// 返回成功加载数量；单个失败记录日志并继续。
    core::Result<size_t> loadDirectory(const std::string& dir);

    /// 加载环境变量 ZENITH_PLUGIN_PATH 中的全部目录。
    core::Result<size_t> loadFromEnvPath();

    core::Result<void> startAll();
    core::Result<void> stopAll();
    void unloadAll();

    [[nodiscard]] const std::vector<std::unique_ptr<PluginInfo>>& plugins() const noexcept {
        return plugins_;
    }

private:
    core::Result<void> validateDesc(const zenith_plugin_desc* desc, const DynLib& lib);

    ServiceRegistry&         registry_;
    concurrency::ThreadPool* pool_;
    core::KeyValueConfig*    config_;
    std::vector<std::unique_ptr<PluginInfo>> plugins_;
};

} // namespace zenith::plugin
