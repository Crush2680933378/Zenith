// ============================================================================
//  01_plugin_host —— Zenith 插件宿主示例
//  完整走一遍 M1 能力闭环：
//    日志装配 → 线程池/注册表/配置 → 加载插件目录 → startAll →
//    消费 C++ 服务 + 纯 C 服务 → stopAll/unloadAll（安全卸载）→ 收尾
// ============================================================================
#include <memory>
#include <thread>

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/log/async_file_logger.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

#include "greeter.hpp"

#ifndef ZENITH_EXAMPLE_PLUGIN_DIR
#define ZENITH_EXAMPLE_PLUGIN_DIR "./plugins"
#endif

int main() {
    using namespace zenith;

    /* ---- 1. 日志装配：控制台（彩色）+ 异步滚动文件 ---- */
    auto& facade = log::LoggerFacade::instance();
    facade.setLevel(log::LogLevel::Trace);
    facade.addSink(std::make_shared<log::ConsoleLogger>(/*useStderr=*/true, /*color=*/true));
    facade.addSink(std::make_shared<log::AsyncFileLogger>("zenith_host.log"));
    ZLOG_INFO("=== Zenith plugin host example ===");

    /* ---- 2. 基础设施 ---- */
    concurrency::ThreadPool pool(4);
    plugin::ServiceRegistry  registry;
    core::KeyValueConfig     config;
    config.set("hello_c.greeting_prefix", "Bonjour");

    /* ---- 3. 加载插件 ---- */
    plugin::PluginManager mgr(registry, &pool, &config);
    auto loaded = mgr.loadDirectory(ZENITH_EXAMPLE_PLUGIN_DIR);
    if (!loaded) {
        ZLOG_ERROR("loadDirectory failed: ", loaded.error().message);
        return 1;
    }
    ZLOG_INFO("loaded ", loaded.value(), " plugin(s)");
    for (const auto& p : mgr.plugins()) {
        ZLOG_INFO("  - ", p->name, " v", p->version, ": ", p->description);
    }

    if (!mgr.startAll()) {
        ZLOG_ERROR("startAll failed");
    } else {
        /* ---- 4. 消费 C++ 虚接口服务（hello_cpp 注册） ---- */
        void* raw = registry.queryRaw(example::IGreeter::service_name(), 1);
        if (raw != nullptr) {
            auto* greeter = static_cast<example::IGreeter*>(raw);
            ZLOG_INFO("IGreeter says: ", greeter->greet("Zenith"));
        } else {
            ZLOG_WARN("IGreeter service not found");
        }

        /* ---- 5. 消费纯 C vtable 服务（hello_c 注册） ---- */
        zenith_status st = ZENITH_OK;
        void* craw = registry.queryRaw(ZENITH_C_GREETER_NAME, ZENITH_C_GREETER_VERSION, &st);
        if (craw != nullptr) {
            auto* cg = static_cast<zenith_c_greeter*>(craw);
            char buf[256];
            if (cg->vt->greet(craw, "Zenith", buf, sizeof(buf)) != nullptr) {
                ZLOG_INFO("CGreeter says: ", buf);
            }
        } else {
            ZLOG_WARN("CGreeter service not found (status=", st, ")");
        }
    }

    /* ---- 6. 停机：先停插件，再清服务，最后卸库 ---- */
    (void)mgr.stopAll();
    mgr.unloadAll();
    ZLOG_INFO("services remaining after unload: ", registry.size());

    pool.shutdown();
    facade.flush();
    ZLOG_INFO("=== bye ===");
    facade.shutdown(); // main 返回前销毁异步 sink（避免 DLL 卸载期 join 死锁）
    return 0;
}
