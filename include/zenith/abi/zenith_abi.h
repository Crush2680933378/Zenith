// ============================================================================
//  Zenith Plugin ABI v1 — 纯 C、编译器无关、跨平台稳定的插件边界契约
//
//  设计红线（详见 docs/ARCHITECTURE.md）：
//    1. 跨动态库边界只允许出现 C 类型（POD、指针、定宽整数、函数指针），
//       禁止 STL 类型与 C++ 虚函数穿越本边界。
//    2. 所有结构体以 struct_size + abi_version 开头，实现前向兼容：
//       新版本只能向尾部追加字段，读方对未知字段必须忽略。
//    3. 每个插件全局只导出一个符号 zenith_plugin_entry()。
//    4. 本头文件必须能被 C89/C99/C++、MSVC/GCC/Clang/xtensa-gcc 无警告编译。
//
//  插件生命周期（宿主驱动）：
//    load → init(host) → start() → [运行期] → stop() → shutdown() → 卸载
//    - init    ：必需。宿主注入能力表 host；插件在此注册服务（回传
//                host->plugin_token 作为所有权令牌）。
//    - start   ：可空。进入工作状态。
//    - stop    ：可空。停止接收新工作，尽快收敛。
//    - shutdown：可空。释放 init 期间申请、且未注册给注册表的资源。
//    宿主在卸载动态库之前，会先按 plugin_token 清理该插件注册的全部
//    服务并调用其 destroy 回调，因此 destroy 的代码执行时库必然还在。
// ============================================================================
#ifndef ZENITH_ABI_H
#define ZENITH_ABI_H

#include <stddef.h>   /* size_t, offsetof */
#include <stdint.h>   /* 定宽整数 */

/// 当前 ABI 版本。插件声明的 abi_version 必须 <= 此值。
#define ZENITH_ABI_VERSION 1u

#if defined(_WIN32)
  #define ZENITH_EXPORT __declspec(dllexport)
#else
  #define ZENITH_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// 状态码：宿主与插件之间唯一的错误语言
// ---------------------------------------------------------------------------
typedef enum zenith_status {
    ZENITH_OK                     = 0,
    ZENITH_ERR_INVALID_ARGUMENT   = 1,
    ZENITH_ERR_NOT_FOUND          = 2,
    ZENITH_ERR_UNSUPPORTED        = 3,
    ZENITH_ERR_OUT_OF_MEMORY      = 4,
    ZENITH_ERR_VERSION_MISMATCH   = 5,
    ZENITH_ERR_ALREADY_EXISTS     = 6,
    ZENITH_ERR_NOT_INITIALIZED    = 7,
    ZENITH_ERR_PLUGIN_INIT_FAILED = 8,
    ZENITH_ERR_GENERIC            = 0x7FFFFFFF
} zenith_status;

/// 日志级别。数值与 zenith::log::LogLevel 一一对应。
typedef enum zenith_log_level {
    ZENITH_LOG_TRACE = 0,
    ZENITH_LOG_DEBUG = 1,
    ZENITH_LOG_INFO  = 2,
    ZENITH_LOG_WARN  = 3,
    ZENITH_LOG_ERROR = 4,
    ZENITH_LOG_FATAL = 5
} zenith_log_level;

struct zenith_host_api;
typedef struct zenith_host_api zenith_host_api;      /* 当前布局即 v1 */
typedef zenith_host_api       zenith_host_api_v1;    /* v2 出现后供源码显式锁版 */

/// C 侧服务实例析构函数：插件提供，宿主在“库仍加载”的安全窗口内调用。
typedef void (*zenith_destroy_fn)(void* instance);

// ---------------------------------------------------------------------------
// 宿主能力表：以函数指针 + 各自独立的 ctx 注入插件。
// 所有 ctx 对插件完全不透明；除 plugin_token 外插件不得缓存任何指针
// 生命周期超出一次 init 周期的字段。
// ---------------------------------------------------------------------------
struct zenith_host_api {
    uint32_t struct_size;    /* == sizeof(zenith_host_api)，前向兼容校验 */
    uint32_t abi_version;    /* == ZENITH_ABI_VERSION                     */

    /// 宿主注入的本插件身份令牌。注册服务时必须原样回传，
    /// 宿主据此在卸载前安全清理本插件注册的所有服务。
    void* plugin_token;

    /* ---- 日志 ----------------------------------------------------------- */
    void* log_ctx;           /* 保留：当前指向 NULL，走进程全局日志门面 */
    void  (*log_write)(void* log_ctx, int level, const char* file, int line,
                       const char* msg);

    /* ---- 内存（可选统计钩子，插件可自由改用自己的分配器） ----------------- */
    void* (*mem_alloc)(size_t size);
    void  (*mem_free)(void* ptr);

    /* ---- 服务注册表 ------------------------------------------------------ */
    void* registry_ctx;
    zenith_status (*register_service)(void* registry_ctx, void* owner_token,
                                      const char* interface_name,
                                      uint32_t impl_version,
                                      void* instance,
                                      zenith_destroy_fn destroy_instance /* 可 NULL */);
    zenith_status (*query_service)(void* registry_ctx, const char* interface_name,
                                   uint32_t min_version, void** out_instance);

    /* ---- 配置 ------------------------------------------------------------ */
    void* config_ctx;
    zenith_status (*config_get)(void* config_ctx, const char* key,
                                const char* default_value, const char** out_value);

    /* ---- 线程池任务派发 --------------------------------------------------- */
    void* pool_ctx;
    zenith_status (*post_task)(void* pool_ctx, void (*fn)(void* arg), void* arg);

    /* ---- 预留：v1.x 追加能力时使用，读方使用前必须判空 ------------------- */
    void* reserved[4];
};

// ---------------------------------------------------------------------------
// 插件描述符：zenith_plugin_entry() 返回指向静态存储期的本结构。
// ---------------------------------------------------------------------------
typedef struct zenith_plugin_desc {
    uint32_t    struct_size;   /* 宿主校验 >= ZENITH_DESC_MIN_SIZE */
    uint32_t    abi_version;   /* 编译时面向的 ABI 版本，须 <= ZENITH_ABI_VERSION */

    const char* name;          /* 唯一短名，如 "media-ffmpeg"（非 NULL） */
    const char* version;       /* 语义化版本字符串，如 "0.1.0" */
    const char* description;   /* 可 NULL */
    const char* author;        /* 可 NULL */

    zenith_status (*init)(const struct zenith_host_api* host); /* 必需，非 NULL */
    zenith_status (*start)(void);                               /* 可 NULL */
    zenith_status (*stop)(void);                                /* 可 NULL */
    void          (*shutdown)(void);                            /* 可 NULL */

    void* reserved[4];
} zenith_plugin_desc;

/// 宿主接受的描述符最小尺寸（核心字段到 reserved 之前）。
#define ZENITH_DESC_MIN_SIZE \
    ((uint32_t)offsetof(zenith_plugin_desc, reserved))

/// 唯一入口符号：宿主在动态库中查找此名字。
ZENITH_EXPORT const zenith_plugin_desc* zenith_plugin_entry(void);

#if defined(__cplusplus)
/// C++ 插件便捷导出宏。用法：
///   static zenith_plugin_desc g_desc = { ... };
///   ZENITH_PLUGIN_EXPORT(g_desc)
  #define ZENITH_PLUGIN_EXPORT(desc_var)                                          \
    extern "C" ZENITH_EXPORT const zenith_plugin_desc* zenith_plugin_entry(void)  \
    {                                                                             \
        return &(desc_var);                                                       \
    }
#endif

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ZENITH_ABI_H */
