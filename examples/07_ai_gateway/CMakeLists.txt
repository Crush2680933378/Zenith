# 07_ai_gateway —— AI 推理插件 → OpenAI 兼容网关（ZenithDesk 的后端）

add_executable(ai_gateway main.cpp)

target_link_libraries(ai_gateway PRIVATE zenith Threads::Threads)

target_compile_definitions(ai_gateway PRIVATE
    ZENITH_EXAMPLE_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")

zenith_set_warnings(ai_gateway)
