# ai_llama —— 进程内 llama.cpp 插件（纯 C ABI 静态链接）
#
# llama.cpp 由源码编译（MinGW / CPU 后端）后装在 ZENITH_LLAMA_ROOT：
#   <root>/include/llama.h + ggml*.h
#   <root>/lib/libllama.a ggml.a ggml-base.a ggml-cpu.a
#
# 换成 MSVC + CUDA 构建时，只需重编 llama.cpp 并把它 install 到另一个前缀，
# 再用 -DZENITH_LLAMA_ROOT=<新前缀> 配置即可（接口不变，源码一行不用改）。

set(ZENITH_LLAMA_ROOT
    "C:/Users/26809/Desktop/Code/c++/_deps/llama-install-mingw"
    CACHE PATH "llama.cpp install prefix (include/llama.h + lib/libllama.a)")

if (NOT EXISTS "${ZENITH_LLAMA_ROOT}/include/llama.h")
    message(WARNING
        "ai_llama: ${ZENITH_LLAMA_ROOT}/include/llama.h not found — plugin disabled. "
        "Build llama.cpp first and pass -DZENITH_LLAMA_ROOT=<prefix>.")
    return()
endif()

add_library(zenith_ai_llama MODULE ai_llama.cpp)

set_target_properties(zenith_ai_llama PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_ai_llama PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/abi)

# 第三方头（llama.h 会带进 ggml 头）按 SYSTEM 引入，不污染本仓的零警告基线
target_include_directories(zenith_ai_llama SYSTEM PRIVATE ${ZENITH_LLAMA_ROOT}/include)

# 静态链接顺序：llama → ggml → ggml-base → ggml-cpu（依赖倒序）
target_link_libraries(zenith_ai_llama PRIVATE
    "${ZENITH_LLAMA_ROOT}/lib/libllama.a"
    "${ZENITH_LLAMA_ROOT}/lib/ggml.a"
    "${ZENITH_LLAMA_ROOT}/lib/ggml-base.a"
    "${ZENITH_LLAMA_ROOT}/lib/ggml-cpu.a")

# ggml 默认带 GGML_OPENMP=ON：静态库里有 GOMP_* 引用，必须把运行时链接进来
find_package(OpenMP REQUIRED)
target_link_libraries(zenith_ai_llama PRIVATE OpenMP::OpenMP_CXX)

if (WIN32)
    target_link_libraries(zenith_ai_llama PRIVATE ws2_32)
endif()

zenith_set_warnings(zenith_ai_llama)
