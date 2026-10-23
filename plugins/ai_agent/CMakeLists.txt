# ai_agent —— Agent 运行时插件（内联 agent.cpp）
add_library(zenith_ai_agent MODULE ai_agent.cpp)
set_target_properties(zenith_ai_agent PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_agent PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_agent PRIVATE zenith_core)
if (WIN32)
    target_link_libraries(zenith_ai_agent PRIVATE ws2_32)
endif()
zenith_set_warnings(zenith_ai_agent)
