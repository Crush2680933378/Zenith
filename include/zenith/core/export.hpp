// ============================================================================
//  Zenith 动态库导出宏（Windows 三态）
//    - 构建 zenith 动态库：ZENITH_BUILDING_LIBRARY（CMake 注入）→ dllexport
//    - 构建/消费 zenith_core 静态库：ZENITH_STATIC（CMake 注入）→ 空
//    - 消费 zenith 动态库：默认 → dllimport
// ============================================================================
#pragma once

#if defined(_WIN32)
  #if defined(ZENITH_BUILDING_LIBRARY)
    #define ZENITH_API __declspec(dllexport)
  #elif defined(ZENITH_STATIC)
    #define ZENITH_API
  #else
    #define ZENITH_API __declspec(dllimport)
  #endif
  #define ZENITH_LOCAL
#else
  #define ZENITH_API __attribute__((visibility("default")))
  #define ZENITH_LOCAL __attribute__((visibility("hidden")))
#endif
