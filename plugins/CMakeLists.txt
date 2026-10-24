# 示例插件：全部输出到 build/plugins，供宿主与端到端测试加载
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZENITH_PLUGIN_OUTPUT_DIR})
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ZENITH_PLUGIN_OUTPUT_DIR})

add_subdirectory(hello_cpp)
add_subdirectory(hello_c)
add_subdirectory(media_ffmpeg)
add_subdirectory(edge_esp32)
add_subdirectory(ai_comfyui)
add_subdirectory(ai_inference)
add_subdirectory(ai_llama)     # 进程内 llama.cpp（纯 C ABI 静态链接）
add_subdirectory(ai_ninfer)    # NInfer 引擎（无 C ABI ⇒ 进程监管 + HTTP）
add_subdirectory(db_vector)
add_subdirectory(ai_mcp)
add_subdirectory(ai_agent)
add_subdirectory(ai_crew)
add_subdirectory(ai_router)
add_subdirectory(ai_guard)   # LLM 输入护栏（注入/PII）
add_subdirectory(ai_speech)  # STT/TTS 接口 + mock
