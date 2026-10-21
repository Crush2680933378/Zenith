# ai_ninfer —— NInfer 引擎专用插件（**无 C ABI ⇒ 进程监管 + HTTP**）
#
# 复用 ai_inference 里的三个源文件（同一份代码，不复制）：
#   engine_supervisor.cpp / upstream_http.cpp / openai_upstream_backend.cpp

add_library(zenith_ai_ninfer MODULE
    ai_ninfer.cpp
    ../ai_inference/engine_supervisor.cpp
    ../ai_inference/upstream_http.cpp
    ../ai_inference/openai_upstream_backend.cpp)

set_target_properties(zenith_ai_ninfer PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_ai_ninfer PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/abi
    ${CMAKE_SOURCE_DIR}/plugins/ai_inference)

# JSON 复用核心库的自研解析器（json.cpp 静态编入本模块）
target_link_libraries(zenith_ai_ninfer PRIVATE zenith_core)

if (WIN32)
    target_link_libraries(zenith_ai_ninfer PRIVATE ws2_32)
endif()

zenith_set_warnings(zenith_ai_ninfer)
