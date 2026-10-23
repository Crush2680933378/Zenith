# ai_mcp —— MCP 网关插件（内联 mcp_server/mcp_client 编译）
add_library(zenith_ai_mcp MODULE ai_mcp.cpp)
set_target_properties(zenith_ai_mcp PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_mcp PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_mcp PRIVATE zenith_core)
if (WIN32)
    target_link_libraries(zenith_ai_mcp PRIVATE ws2_32)
endif()
zenith_set_warnings(zenith_ai_mcp)
