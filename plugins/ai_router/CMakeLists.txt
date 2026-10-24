# ai_router —— 多厂商 LLM 路由插件（链 zenith_core；provider_router.cpp 已入核心）
add_library(zenith_ai_router MODULE ai_router.cpp)
set_target_properties(zenith_ai_router PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_router PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_router PRIVATE zenith_core)
zenith_set_warnings(zenith_ai_router)
