#include "zenith/plugin/plugin_manager.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "zenith/core/string.hpp"
#include "zenith/log/logger.hpp"

namespace zenith::plugin {

namespace fs = std::filesystem;

PluginManager::PluginManager(ServiceRegistry& registry, concurrency::ThreadPool* pool,
                             core::KeyValueConfig* config)
    : registry_(registry), pool_(pool), config_(config) {}

PluginManager::~PluginManager() { unloadAll(); }

core::Result<PluginInfo*> PluginManager::load(const std::string& path) {
    auto info = std::make_unique<PluginInfo>();
    info->path = path;
    info->lib  = std::make_unique<DynLib>(path);

    auto lr = info->lib->load();
    if (!lr) {
        return core::Result<PluginInfo*>::Err(std::move(lr).error());
    }

    using EntryFn = const zenith_plugin_desc* (*)(void);
    auto entryFn = reinterpret_cast<EntryFn>(info->lib->symbol("zenith_plugin_entry"));
    if (entryFn == nullptr) {
        return core::Result<PluginInfo*>::Err(
            core::Error(3, "not a Zenith plugin (entry symbol missing): " + path));
    }
    const zenith_plugin_desc* entry = entryFn(); // 调用入口函数取描述符
    if (entry == nullptr) {
        return core::Result<PluginInfo*>::Err(
            core::Error(3, "plugin entry returned null descriptor: " + path));
    }

    auto vr = validateDesc(entry, *info->lib);
    if (!vr) {
        return core::Result<PluginInfo*>::Err(std::move(vr).error());
    }
    info->desc        = *entry;
    info->name        = entry->name ? entry->name : "(unnamed)";
    info->version     = entry->version ? entry->version : "0.0.0";
    info->description = entry->description ? entry->description : "";
    info->author      = entry->author ? entry->author : "";
    info->state       = PluginState::Loaded;

    ZLOG_INFO("plugin loaded: ", info->name, " v", info->version, " (", path, ")");

    // init：注入携带本插件令牌的宿主能力表（存于 PluginInfo，栈生存期外仍有效）
    info->hostApi = makeHostApiV1(HostContext{&registry_, pool_, config_},
                                   static_cast<void*>(info.get()));
    const zenith_status st = info->desc.init(&info->hostApi);
    if (st != ZENITH_OK) {
        info->lib->unload();
        return core::Result<PluginInfo*>::Err(
            core::Error(4, "plugin init failed (" + std::to_string(st) + "): " + info->name));
    }
    info->state = PluginState::Initialized;
    plugins_.push_back(std::move(info));
    return core::Result<PluginInfo*>::Ok(plugins_.back().get());
}

core::Result<size_t> PluginManager::loadDirectory(const std::string& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        return core::Result<size_t>::Err(core::Error(5, "plugin dir not found: " + dir));
    }

    const std::string ext = pluginFileExtension();
    std::vector<std::string> files;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string p = entry.path().string();
        const std::string e = core::string_util::toLower(entry.path().extension().string());
        if (e == ext || (ext == ".so" && e == ".so")) files.push_back(p);
    }
    std::sort(files.begin(), files.end());

    size_t ok = 0;
    for (const std::string& f : files) {
        auto r = load(f);
        if (r) {
            ++ok;
        } else {
            ZLOG_ERROR("skip plugin ", f, ": ", r.error().message);
        }
    }
    return core::Result<size_t>::Ok(ok);
}

core::Result<size_t> PluginManager::loadFromEnvPath() {
    const char* env = std::getenv("ZENITH_PLUGIN_PATH");
    if (env == nullptr || env[0] == '\0') {
        return core::Result<size_t>::Err(core::Error(6, "ZENITH_PLUGIN_PATH not set"));
    }
#if defined(ZENITH_PLATFORM_WINDOWS)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t total = 0;
    for (const std::string& dir : core::string_util::split(env, sep, /*keepEmpty=*/false)) {
        auto r = loadDirectory(dir);
        if (r) total += r.value();
    }
    return core::Result<size_t>::Ok(total);
}

core::Result<void> PluginManager::startAll() {
    for (auto& p : plugins_) {
        if (p->state != PluginState::Initialized) continue;
        if (p->desc.start == nullptr) {
            p->state = PluginState::Started;
            continue;
        }
        const zenith_status st = p->desc.start();
        if (st != ZENITH_OK) {
            p->state = PluginState::Initialized;
            return core::Result<void>::Err(
                core::Error(7, "plugin start failed (" + std::to_string(st) + "): " + p->name));
        }
        p->state = PluginState::Started;
        ZLOG_INFO("plugin started: ", p->name);
    }
    return core::Result<void>::Ok();
}

core::Result<void> PluginManager::stopAll() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        PluginInfo& p = **it;
        if (p.state != PluginState::Started) continue;
        if (p.desc.stop != nullptr) {
            const zenith_status st = p.desc.stop();
            if (st != ZENITH_OK) {
                ZLOG_WARN("plugin stop returned ", st, ": ", p.name);
            }
        }
        p.state = PluginState::Stopped;
        ZLOG_INFO("plugin stopped: ", p.name);
    }
    return core::Result<void>::Ok();
}

void PluginManager::unloadAll() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        PluginInfo& p = **it;
        if (p.state == PluginState::Started) {
            if (p.desc.stop != nullptr) p.desc.stop();
            p.state = PluginState::Stopped;
        }
        // 1) 先清理本插件注册的服务（destroy 执行时代码仍在内存）
        const size_t n = registry_.removeByOwner(static_cast<void*>(&p));
        // 2) 再回调 shutdown / 卸载动态库
        if (p.desc.shutdown != nullptr && p.state >= PluginState::Initialized) {
            p.desc.shutdown();
        }
        if (p.lib != nullptr) p.lib->unload();
        p.state = PluginState::Unloaded;
        ZLOG_INFO("plugin unloaded: ", p.name, " (services cleaned: ", n, ")");
    }
    plugins_.clear();
}

core::Result<void> PluginManager::validateDesc(const zenith_plugin_desc* desc,
                                               const DynLib& lib) {
    if (desc == nullptr) {
        return core::Result<void>::Err(core::Error(8, lib.path() + ": null descriptor"));
    }
    if (desc->struct_size < ZENITH_DESC_MIN_SIZE) {
        return core::Result<void>::Err(
            core::Error(8, lib.path() + ": desc.struct_size too small (" +
                               std::to_string(desc->struct_size) + " < " +
                               std::to_string(ZENITH_DESC_MIN_SIZE) + ")"));
    }
    if (desc->abi_version == 0 || desc->abi_version > ZENITH_ABI_VERSION) {
        return core::Result<void>::Err(
            core::Error(8, lib.path() + ": ABI version mismatch (plugin " +
                               std::to_string(desc->abi_version) + ", host " +
                               std::to_string(ZENITH_ABI_VERSION) + ")"));
    }
    if (desc->init == nullptr) {
        return core::Result<void>::Err(
            core::Error(8, lib.path() + ": desc.init is required"));
    }
    return core::Result<void>::Ok();
}

} // namespace zenith::plugin
