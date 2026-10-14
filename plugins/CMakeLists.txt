# 示例插件：全部输出到 build/plugins，供宿主与端到端测试加载
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZENITH_PLUGIN_OUTPUT_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ZENITH_PLUGIN_OUTPUT_DIR})

add_subdirectory(hello_cpp)
add_subdirectory(hello_c)
