# ai_inference —— LLM 推理网关插件（M8.1，2026-10 重写）
# 形态：进程监管本地引擎（NInfer / llama-server）+ OpenAI 协议对接。
# 不再内置进程内 llama.cpp：两套发行版都只有二进制（无 llama.h/导入库），
# 且 KV 均值中心只在 server 层（详见 docs/PLAN_M8.md M8.1 状态块）。

add_library(zenith_ai_inference MODULE
    ai_inference.cpp
    engine_supervisor.cpp
    mock_backend.cpp
    openai_upstream_backend.cpp
    upstream_http.cpp)

set_target_properties(zenith_ai_inference PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_ai_inference PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/abi
    ${CMAKE_SOURCE_DIR}/plugins/ai_inference)

# JSON 复用核心库的自研解析器（json.cpp 静态编入本模块；插件不依赖宿主符号）
target_link_libraries(zenith_ai_inference PRIVATE zenith_core)

if (WIN32)
    target_link_libraries(zenith_ai_inference PRIVATE ws2_32)
endif()

zenith_set_warnings(zenith_ai_inference)
