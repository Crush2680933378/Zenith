// ============================================================================
//  平台 / 编译器识别与通用编译期辅助宏
// ============================================================================
#pragma once

/* ---- 平台 ---- */
#if defined(_WIN32) || defined(_WIN64)
  #define ZENITH_PLATFORM_WINDOWS 1
#elif defined(__APPLE__)
  #include <TargetConditionals.h>
  #define ZENITH_PLATFORM_MACOS 1
#elif defined(__linux__)
  #define ZENITH_PLATFORM_LINUX 1
#else
  #error "Zenith: 未识别的平台，请在 platform.hpp 中适配"
#endif

/* ---- 编译器 ---- */
#if defined(_MSC_VER)
  #define ZENITH_COMPILER_MSVC 1
#elif defined(__clang__)
  #define ZENITH_COMPILER_CLANG 1
#elif defined(__GNUC__)
  #define ZENITH_COMPILER_GCC 1
#else
  #error "Zenith: 未识别的编译器"
#endif

/* ---- 分支预测提示 ---- */
#if defined(ZENITH_COMPILER_GCC) || defined(ZENITH_COMPILER_CLANG)
  #define ZENITH_LIKELY(x)   (__builtin_expect(!!(x), 1))
  #define ZENITH_UNLIKELY(x) (__builtin_expect(!!(x), 0))
#else
  #define ZENITH_LIKELY(x)   (x)
  #define ZENITH_UNLIKELY(x) (x)
#endif

#if defined(ZENITH_COMPILER_MSVC)
  #define ZENITH_FORCEINLINE __forceinline
#else
  #define ZENITH_FORCEINLINE inline __attribute__((always_inline))
#endif

#define ZENITH_UNUSED(x) (void)(x)
