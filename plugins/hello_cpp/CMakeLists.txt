# hello_cpp —— C++ 示例插件（MODULE：只导出 zenith_plugin_entry，不链接 zenith）
add_library(zenith_hello_cpp MODULE zenith_hello_cpp.cpp)

set_target_properties(zenith_hello_cpp PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_hello_cpp PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/common)

zenith_set_warnings(zenith_hello_cpp)
