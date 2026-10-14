# hello_c —— 纯 C 示例插件（用 C 编译器编译，验证纯 C ABI 可用性）
add_library(zenith_hello_c MODULE zenith_hello_c.c)

set_target_properties(zenith_hello_c PROPERTIES
    PREFIX "" LINKER_LANGUAGE C C_STANDARD 11 C_STANDARD_REQUIRED ON)

target_include_directories(zenith_hello_c PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/common)

zenith_set_warnings(zenith_hello_c)
