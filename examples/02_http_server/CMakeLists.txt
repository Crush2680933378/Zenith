# 02_http_server —— Gin 风格 HTTP 服务示例
add_executable(http_server main.cpp)

target_link_libraries(http_server PRIVATE zenith Threads::Threads)

target_compile_definitions(http_server PRIVATE
    ZENITH_EXAMPLE_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")

zenith_set_warnings(http_server)
