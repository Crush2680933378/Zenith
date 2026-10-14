# 01_plugin_host —— 插件宿主示例（链接动态库 zenith，验证 DLL 形态）
add_executable(plugin_host main.cpp)

target_link_libraries(plugin_host PRIVATE zenith Threads::Threads)

target_include_directories(plugin_host PRIVATE ${CMAKE_SOURCE_DIR}/examples/common)

target_compile_definitions(plugin_host PRIVATE
    ZENITH_EXAMPLE_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")

zenith_set_warnings(plugin_host)
