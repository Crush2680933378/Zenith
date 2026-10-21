// ============================================================================
//  plugin_entry.cpp —— Zenith 插件外壳（MSVC 编译，与 CUDA 引擎同侧）
//
//  它把 ninfer_c_shim.cpp 的纯 C 函数装成一张 **C 服务表**（zenith_nz_service），
//  以 zenith_plugin_entry 导出。宿主（MinGW）用 zenith_plugin_desc 结构加载，
//  再按名字取到这张表——全程没有 C++ 对象跨编译器，这正是 Zenith C ABI 内核的用法。
//
//  与 ai_ninfer（子进程 + HTTP）的区别：本插件**在宿主进程内**跑引擎，无 HTTP 跳、
//  无子进程；代价是引擎崩溃＝宿主崩溃。
// ============================================================================
#include <cstdlib>
#include <cstring>
#include <string>

#include "zenith/abi/zenith_abi.h"
#include "zenith/ai/ninfer_c_api.h"

// ---- shim 导出的 C 函数 -----------------------------------------------------
extern "C" {
zenith_nz_engine* zenith_nz_create(const char* artifact_path, const zenith_nz_config* cfg);
const char* zenith_nz_last_create_error(void);
void zenith_nz_destroy(zenith_nz_engine* e);
const char* zenith_nz_last_error(zenith_nz_engine* e);
int32_t zenith_nz_generate(zenith_nz_engine* e, const zenith_nz_message* messages, int32_t count,
                           int32_t max_tokens, const zenith_nz_sampling* sampling,
                           zenith_nz_token_cb cb, void* user, zenith_nz_result* out);
int32_t zenith_nz_stats(zenith_nz_engine* e, uint64_t* requests, uint64_t* prompt_tokens,
                        uint64_t* completion_tokens, uint64_t* accepted, uint64_t* drafted);
}

namespace {

const zenith_host_api* g_host = nullptr;

/// 服务表：静态存储，注册给宿主的就是这个指针
zenith_nz_service g_service = {};

void logAt(zenith_log_level level, const std::string& m) {
    if (g_host != nullptr && g_host->log_write != nullptr) {
        g_host->log_write(g_host->log_ctx, level, "ai_ninfer_native", 0, m.c_str());
    }
}

std::string cfgStr(const char* key, const char* fallback) {
    const char* v = nullptr;
    if (g_host != nullptr && g_host->config_get != nullptr &&
        g_host->config_get(g_host->config_ctx, key, "", &v) == ZENITH_OK && v != nullptr) {
        return std::string(v);
    }
    return std::string(fallback);
}

int cfgInt(const char* key, int fallback) {
    const std::string s = cfgStr(key, "");
    return s.empty() ? fallback : std::atoi(s.c_str());
}

int cfgU32(const char* key, unsigned fallback) {
    const std::string s = cfgStr(key, "");
    return s.empty() ? static_cast<int>(fallback) : std::atoi(s.c_str());
}

// ---- 服务表里的三个转发（签名差一个 void*）---------------------------------
int32_t NZ_CALL svcGenerate(void* engine, const zenith_nz_message* messages, int32_t count,
                            int32_t max_tokens, const zenith_nz_sampling* sampling,
                            zenith_nz_token_cb cb, void* user, zenith_nz_result* out) {
    return zenith_nz_generate(static_cast<zenith_nz_engine*>(engine), messages, count, max_tokens,
                              sampling, cb, user, out);
}

int32_t NZ_CALL svcStats(void* engine, uint64_t* requests, uint64_t* prompt_tokens,
                         uint64_t* completion_tokens, uint64_t* accepted, uint64_t* drafted) {
    return zenith_nz_stats(static_cast<zenith_nz_engine*>(engine), requests, prompt_tokens,
                           completion_tokens, accepted, drafted);
}

const char* NZ_CALL svcLastError(void* engine) {
    return zenith_nz_last_error(static_cast<zenith_nz_engine*>(engine));
}

/// 插件卸载时销毁引擎（释放显存），服务表本身是静态的
void destroyService(void*) {
    if (g_service.engine != nullptr) {
        zenith_nz_destroy(static_cast<zenith_nz_engine*>(g_service.engine));
        g_service.engine = nullptr;
    }
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    const std::string model = cfgStr("ai.ninfer_native.model", "");
    if (model.empty()) {
        logAt(ZENITH_LOG_ERROR, "ai_ninfer_native: ai.ninfer_native.model is empty");
        return ZENITH_ERR_INVALID_ARGUMENT;
    }

    zenith_nz_config cfg{};
    cfg.struct_size = sizeof(zenith_nz_config);
    cfg.device = cfgInt("ai.ninfer_native.device", 0);
    cfg.max_context = static_cast<uint32_t>(cfgU32("ai.ninfer_native.max_context", 16384));
    cfg.kv_capacity_tokens =
        static_cast<uint32_t>(cfgU32("ai.ninfer_native.kv_tokens", cfg.max_context));
    cfg.prefill_chunk = static_cast<uint32_t>(cfgU32("ai.ninfer_native.prefill_chunk", 1024));
    cfg.kv_dtype = static_cast<uint32_t>(cfgU32("ai.ninfer_native.kv_dtype", ZENITH_NZ_KV_K8V4));
    cfg.spec_backend =
        static_cast<uint32_t>(cfgU32("ai.ninfer_native.spec", ZENITH_NZ_SPEC_MTP));
    cfg.draft_tokens = static_cast<uint32_t>(cfgU32("ai.ninfer_native.draft_tokens", 4));
    cfg.max_concurrency = static_cast<uint32_t>(cfgU32("ai.ninfer_native.max_concurrency", 1));
    cfg.gdn_state_fp16 = static_cast<uint32_t>(cfgU32("ai.ninfer_native.gdn_state_fp16", 0));

    logAt(ZENITH_LOG_INFO, "ai_ninfer_native: loading engine (in-process) from " + model);
    zenith_nz_engine* engine = zenith_nz_create(model.c_str(), &cfg);
    if (engine == nullptr) {
        const char* why = zenith_nz_last_create_error();
        const std::string detail = why != nullptr ? why : "(no detail)";
        logAt(ZENITH_LOG_ERROR, "ai_ninfer_native: engine create failed: " + detail);
        // 宿主可能没有装日志 sink；测试/CI 里 stderr 才是唯一可见通道
        std::fprintf(stderr, "[ai_ninfer_native] engine create failed: %s\n", detail.c_str());
        return ZENITH_ERR_PLUGIN_INIT_FAILED;
    }

    g_service.version = ZENITH_NINFER_C_API_VERSION;
    g_service.engine = engine;
    g_service.generate = &svcGenerate;
    g_service.stats = &svcStats;
    g_service.last_error = &svcLastError;

    logAt(ZENITH_LOG_INFO, "ai_ninfer_native: engine ready (in-process, no C++ across boundary)");
    return host->register_service(host->registry_ctx, host->plugin_token, "zenith.ai.INinferCApi", 1,
                                  static_cast<void*>(&g_service), &destroyService);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_ninfer_native",
    "0.1.0",
    "Zenith AI plugin: NInfer engine IN-PROCESS via a self-made pure C ABI shim (MSVC side)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
