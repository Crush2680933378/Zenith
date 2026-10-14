// ============================================================================
//  hello_cpp —— C++ 示例插件
//  演示：宿主日志 / 服务注册（带 owner 令牌与 destroy）/ 线程池任务派发
// ============================================================================
#include <new>
#include <string>

#include "zenith/abi/zenith_abi.h"

#include "greeter.hpp"

namespace {

const zenith_host_api* g_host = nullptr;

class GreeterImpl final : public zenith::example::IGreeter {
public:
    [[nodiscard]] std::string greet(const std::string& name) const override {
        return "Hello, " + name + "! (from hello_cpp plugin)";
    }
};

void destroyGreeter(void* instance) {
    delete static_cast<GreeterImpl*>(instance);
}

void demoTask(void* /*arg*/) {
    if (g_host == nullptr) return;
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_cpp: task is running on the host thread pool");
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                    "hello_cpp: init (abi v" "1" ")");

    // 注册 C++ 虚接口服务：实例在插件堆上，destroy 回传给宿主安全调用
    auto* greeter = new (std::nothrow) GreeterImpl();
    if (greeter == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;

    zenith_status st = host->register_service(
        host->registry_ctx, host->plugin_token,
        zenith::example::IGreeter::service_name(),
        zenith::example::IGreeter::service_version(),
        static_cast<void*>(greeter), &destroyGreeter);
    if (st != ZENITH_OK) {
        delete greeter;
        return st;
    }

    // 演示通过宿主线程池执行后台工作
    st = host->post_task(host->pool_ctx, &demoTask, nullptr);
    return st;
}

zenith_status pluginStart(void) {
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_cpp: start");
    return ZENITH_OK;
}

zenith_status pluginStop(void) {
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_cpp: stop");
    return ZENITH_OK;
}

void pluginShutdown(void) {
    g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__,
                      "hello_cpp: shutdown");
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "hello_cpp",
    "0.1.0",
    "Zenith C++ sample plugin: registers a C++ IGreeter service",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
