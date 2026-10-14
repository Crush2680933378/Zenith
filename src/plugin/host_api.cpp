#include "zenith/plugin/host_api.hpp"

#include <cstdlib>
#include <cstring>

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/plugin/service_registry.hpp"

namespace zenith::plugin {
namespace {

// -------------------------------------------------------------------------
// C thunk：签名与 abi/zenith_abi.h 完全一致，只做 C++ 侧转发
// -------------------------------------------------------------------------

void hostLogWrite(void* /*log_ctx*/, int level, const char* file, int line, const char* msg) {
    if (level < static_cast<int>(log::LogLevel::Trace) ||
        level > static_cast<int>(log::LogLevel::Fatal)) {
        level = static_cast<int>(log::LogLevel::Info);
    }
    log::LoggerFacade::instance().writeRaw(static_cast<log::LogLevel>(level),
                                           file ? file : "?", line, msg);
}

void* hostMemAlloc(size_t size) { return std::malloc(size); }

void hostMemFree(void* ptr) { std::free(ptr); }

zenith_status hostRegisterService(void* registry_ctx, void* owner_token,
                                  const char* interface_name, uint32_t impl_version,
                                  void* instance, zenith_destroy_fn destroy_instance) {
    if (registry_ctx == nullptr) return ZENITH_ERR_NOT_INITIALIZED;
    return static_cast<ServiceRegistry*>(registry_ctx)
        ->registerRaw(interface_name, impl_version, instance, destroy_instance, owner_token,
                      /*replace=*/false);
}

zenith_status hostQueryService(void* registry_ctx, const char* interface_name,
                               uint32_t min_version, void** out_instance) {
    if (out_instance == nullptr) return ZENITH_ERR_INVALID_ARGUMENT;
    if (registry_ctx == nullptr) return ZENITH_ERR_NOT_INITIALIZED;
    zenith_status st = ZENITH_OK;
    *out_instance = static_cast<ServiceRegistry*>(registry_ctx)
                        ->queryRaw(interface_name, min_version, &st);
    return st;
}

zenith_status hostConfigGet(void* config_ctx, const char* key, const char* default_value,
                            const char** out_value) {
    if (out_value == nullptr || key == nullptr) return ZENITH_ERR_INVALID_ARGUMENT;
    if (config_ctx == nullptr) return ZENITH_ERR_NOT_INITIALIZED;

    auto* cfg = static_cast<core::KeyValueConfig*>(config_ctx);
    std::string v;
    if (cfg->tryGet(key, v)) {
        // KeyValueConfig 值在下次 set/remove 前保持稳定；插件应即刻复制使用
        static thread_local std::string tls_value;
        tls_value = std::move(v);
        *out_value = tls_value.c_str();
        return ZENITH_OK;
    }
    *out_value = default_value;
    return ZENITH_OK; // 缺失时回落默认值，不算错误
}

void hostTaskTrampoline(void* arg) {
    auto* fn = static_cast<std::function<void()>*>(arg);
    (*fn)();
    delete fn;
}

zenith_status hostPostTask(void* pool_ctx, void (*fn)(void*), void* arg) {
    if (fn == nullptr) return ZENITH_ERR_INVALID_ARGUMENT;
    if (pool_ctx == nullptr) return ZENITH_ERR_NOT_INITIALIZED;
    auto* pool = static_cast<concurrency::ThreadPool*>(pool_ctx);
    // 直接复用 fn/arg：零额外堆分配
    return pool->post([fn, arg] { fn(arg); }) ? ZENITH_OK : ZENITH_ERR_GENERIC;
}

} // namespace

zenith_host_api makeHostApiV1(const HostContext& ctx, void* ownerToken) {
    zenith_host_api api{};
    api.struct_size    = static_cast<uint32_t>(sizeof(zenith_host_api));
    api.abi_version    = ZENITH_ABI_VERSION;
    api.plugin_token   = ownerToken;

    api.log_ctx        = nullptr;
    api.log_write      = &hostLogWrite;

    api.mem_alloc      = &hostMemAlloc;
    api.mem_free       = &hostMemFree;

    api.registry_ctx   = ctx.registry;
    api.register_service = &hostRegisterService;
    api.query_service    = &hostQueryService;

    api.config_ctx     = ctx.config;
    api.config_get     = &hostConfigGet;

    api.pool_ctx       = ctx.pool;
    api.post_task      = &hostPostTask;

    (void)hostTaskTrampoline; // 预留：无捕获转发器（M2 EventLoop 唤醒复用）
    return api;
}

} // namespace zenith::plugin
