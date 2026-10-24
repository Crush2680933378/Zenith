# ai_guard —— LLM 输入护栏插件（guard_service 已入核心）
add_library(zenith_ai_guard MODULE ai_guard.cpp)
set_target_properties(zenith_ai_guard PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_guard PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_guard PRIVATE zenith_core)
zenith_set_warnings(zenith_ai_guard)
