# ai_comfyui —— ComfyUI AI 绘画插件（MODULE；自包含同步 HTTP 客户端）
add_library(zenith_ai_comfyui MODULE ai_comfyui.cpp)

set_target_properties(zenith_ai_comfyui PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_ai_comfyui PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/common)

if (WIN32)
    target_link_libraries(zenith_ai_comfyui PRIVATE ws2_32)
endif()

zenith_set_warnings(zenith_ai_comfyui)
