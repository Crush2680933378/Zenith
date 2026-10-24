# ai_speech —— 语音接口 + mock 插件（speech_mock 已入核心）
add_library(zenith_ai_speech MODULE ai_speech.cpp)
set_target_properties(zenith_ai_speech PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_include_directories(zenith_ai_speech PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(zenith_ai_speech PRIVATE zenith_core)
zenith_set_warnings(zenith_ai_speech)
