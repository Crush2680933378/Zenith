// ============================================================================
//  zenith/ai/ninfer_c_api.h —— NInfer 引擎的 **纯 C ABI 边界**（M8.1 进程内方案）
//
//  为什么需要这一层：NInfer 只提供 C++ 类 API（ninfer::Engine，
//  std::string_view / std::vector / unique_ptr 跨边界 = 未定义行为），且 CUDA 引擎
//  只能用 MSVC 编译，而 Zenith 宿主是 MinGW。两边唯一安全的交集就是**纯 C**。
//
//  布局规则（违反即崩）：
//    1. 所有参数/返回值都是 C 基本类型、POD 结构、opaque 指针；
//    2. 字符串/数组由**调用方**提供缓冲区，本 API 绝不返回内部分配的指针；
//    3. 回调是 C 函数指针（Win64 上只有一种调用约定）；
//    4. 内存分配与释放必须同侧（nz_* 内部 new/delete 配对）。
//
//  两侧共同包含本头：MSVC 侧（shim/插件）与 MinGW 侧（宿主适配器），
//  结构体布局由编译器按 Itanium/MS 两种 ABI 分别布局 —— 因此**只允许**上面第 1 条
//  的类型，且字段顺序/宽度必须显式固定（用 uint32_t/int32_t/double，不用 size_t/long）。
// ============================================================================
#ifndef ZENITH_AI_NINFER_C_API_H
#define ZENITH_AI_NINFER_C_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  if defined(ZENITH_NINFER_SHIM_BUILD)
#    define NZ_EXPORT __declspec(dllexport)
#  else
#    define NZ_EXPORT
#  endif
#  define NZ_CALL __cdecl
#else
#  define NZ_EXPORT __attribute__((visibility("default")))
#  define NZ_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// 契约版本：布局变更时 +1，宿主据此拒绝不匹配的 shim
#define ZENITH_NINFER_C_API_VERSION 1u

/// 引擎配置（POD，字段全部定宽）
typedef struct zenith_nz_config {
    uint32_t struct_size;        ///< = sizeof(zenith_nz_config)，前向兼容
    int32_t  device;             ///< CUDA 设备号，0 = 第一块卡
    uint32_t max_context;        ///< 单请求逻辑上限，0 = 8192（kv_capacity 须 >= 它）
    uint32_t kv_capacity_tokens; ///< KV 池 token 数，0 = max_context
    uint32_t prefill_chunk;      ///< 预填分块，0 = 引擎默认
    uint32_t kv_dtype;           ///< 见 zenith_nz_kv_dtype
    uint32_t spec_backend;       ///< 见 zenith_nz_spec_backend
    uint32_t draft_tokens;       ///< 投机草稿数（spec_backend != None 时生效）
    uint32_t max_concurrency;    ///< 并发请求数，0 = 1
    uint32_t gdn_state_fp16;     ///< 非 0 用 FP16 GDN 状态（省显存）
} zenith_nz_config;

typedef enum zenith_nz_kv_dtype {
    ZENITH_NZ_KV_BF16 = 0,   ///< KvCacheStorage::BFloat16
    ZENITH_NZ_KV_INT8 = 1,   ///< Int8Group64
    ZENITH_NZ_KV_FP8 = 2,    ///< Fp8E4M3Row256
    ZENITH_NZ_KV_RK8V4 = 3,  ///< RotatedInt8KeyInt4ValueGroup64
    ZENITH_NZ_KV_NVFP4 = 4,  ///< Nvfp4Group16
    ZENITH_NZ_KV_K8V4 = 5,   ///< Fp8KeyNvfp4Value
    ZENITH_NZ_KV_RK4V4 = 6,  ///< RotatedLloyd4KeyInt4Value
    ZENITH_NZ_KV_RK4V4_E8 = 7,
    ZENITH_NZ_KV_RK2V4_E8 = 8,
} zenith_nz_kv_dtype;

typedef enum zenith_nz_spec_backend {
    ZENITH_NZ_SPEC_NONE = 0,
    ZENITH_NZ_SPEC_MTP = 1,
    ZENITH_NZ_SPEC_DFLASH = 2,
    ZENITH_NZ_SPEC_DFLASH2 = 3,
} zenith_nz_spec_backend;

/// 一条消息（role: "system"/"developer"/"user"/"assistant"/"tool"）
typedef struct zenith_nz_message {
    const char* role;
    const char* content;
} zenith_nz_message;

/// 采样参数（全部可选：NaN/负值表示用引擎默认）
typedef struct zenith_nz_sampling {
    uint32_t struct_size;
    float    temperature;      ///< <0 = 默认
    float    top_p;            ///< <=0 = 默认
    int32_t  top_k;            ///< <=0 = 默认
    uint32_t seed;
    int32_t  enable_thinking;  ///< -1 关闭 / 0 引擎默认 / 1 开启
} zenith_nz_sampling;

/// 流式回调：返回 0 继续、非 0 请求中止
typedef int32_t (NZ_CALL *zenith_nz_token_cb)(const char* piece, int32_t len, void* user);

/// 结果（调用方分配；内部只用固定缓冲，绝不交出内部分配的指针）
typedef struct zenith_nz_result {
    uint32_t struct_size;
    int32_t  status;              ///< 0 = 成功，非 0 见 error
    int32_t  finish_reason;       ///< 0=None 1=OutputLimit 2=ContextCapacity 3=StopToken 4=StopString 5=Cancelled
    uint32_t prompt_tokens;
    uint32_t completion_tokens;
    uint32_t reasoning_tokens;
    uint32_t reused_prompt_tokens;
    uint32_t reserved0;
    double   first_token_seconds;
    double   prefill_seconds;
    double   decode_seconds;
    double   total_seconds;
    uint64_t drafted_tokens;
    uint64_t accepted_tokens;
    uint32_t content_len;         ///< 实际写入 content 的字节数（截断时 < 生成长度）
    uint32_t content_truncated;   ///< 非 0 = content 缓冲不够，已截断
    char     content[16384];      ///< 定长缓冲，避免跨 CRT 释放
    char     reasoning[16384]; ///< 思考模型会写很长，2 KB 不够
    char     error[512];
} zenith_nz_result;

/// 引擎句柄（不透明）
typedef struct zenith_nz_engine zenith_nz_engine;

/// 服务表：插件注册的就是这张表的指针（**纯 C，可跨编译器**）
typedef struct zenith_nz_service {
    uint32_t version;  ///< = ZENITH_NINFER_C_API_VERSION
    uint32_t reserved;
    void*    engine;   ///< zenith_nz_engine*（由 shim 创建，插件持有）
    /// 生成一次；返回 0 表示调用成功（结果细节看 out->status）
    int32_t (NZ_CALL* generate)(void* engine, const zenith_nz_message* messages, int32_t count,
                                int32_t max_tokens, const zenith_nz_sampling* sampling,
                                zenith_nz_token_cb cb, void* user, zenith_nz_result* out);
    /// 引擎侧累计计数（请求数/生成 token 数），返回 0 成功
    int32_t (NZ_CALL* stats)(void* engine, uint64_t* requests, uint64_t* prompt_tokens,
                             uint64_t* completion_tokens, uint64_t* accepted, uint64_t* drafted);
    /// 最近一次错误文本（引擎内静态缓冲，调用方不得释放）
    const char* (NZ_CALL* last_error)(void* engine);
} zenith_nz_service;

#ifdef __cplusplus
} // extern "C"
#endif

#endif // ZENITH_AI_NINFER_C_API_H
