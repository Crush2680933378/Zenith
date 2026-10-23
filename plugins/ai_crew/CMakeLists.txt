# ai_crew —— 多 Agent SDLC 集群插件（链 zenith_core；crew.cpp 已入核心库）
add_library(zenith_ai_crew MODULE ai_crew.cpp)
set_target_properties(zenith_ai_crew PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_crew PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_crew PRIVATE zenith_core)
zenith_set_warnings(zenith_ai_crew)
