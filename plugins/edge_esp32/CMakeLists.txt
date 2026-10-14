# edge_esp32 —— 边缘设备桥插件（MODULE；自带 socket/serial 线程，纯 C 边界自治）
add_library(zenith_edge_esp32 MODULE edge_esp32.cpp)

set_target_properties(zenith_edge_esp32 PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_sources(zenith_edge_esp32 PRIVATE
    ${CMAKE_SOURCE_DIR}/src/io/cobs.cpp
    ${CMAKE_SOURCE_DIR}/src/io/serial_port.cpp)

# 内联编译 zenith 源码（非动态链接）：ZENITH_API 须为空，否则 dllimport 冲突
target_compile_definitions(zenith_edge_esp32 PRIVATE ZENITH_STATIC)

target_include_directories(zenith_edge_esp32 PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/common
    ${CMAKE_SOURCE_DIR}/src)

if (WIN32)
    target_link_libraries(zenith_edge_esp32 PRIVATE ws2_32)
endif()

zenith_set_warnings(zenith_edge_esp32)
