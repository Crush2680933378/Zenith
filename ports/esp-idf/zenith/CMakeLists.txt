# Zenith 作为 ESP-IDF 组件接入（ports/esp-idf/zenith）
# 用法（ESP-IDF 工程）：
#   set(EXTRA_COMPONENT_DIRS path/to/Zenith/ports/esp-idf)
#   idf_component_register... 然后 main 里 #include "zenith/..." 直接使用
# 说明：
#   - 编入 core/log/concurrency/plugin 子集（net/http 在 ESP32 走 lwIP 适配，
#     M4.6 提供 select 降级轮询；WebSocket/HTTP 依赖待 lwIP 非阻塞化后续里程碑）
#   - 插件系统：ESP-IDF 无 dlopen——静态注册表替代（同一 C ABI，宿主实现切换）
idf_component_register(
    SRCS
        "src/core/key_value_config.cpp"
        "src/log/logger.cpp"
        "src/log/console_logger.cpp"
        "src/concurrency/thread_pool.cpp"
        "src/io/cobs.cpp"
        "src/plugin/service_registry.cpp"
        "src/plugin/host_api.cpp"
    INCLUDE_DIRS
        "../../../include"
        "../../../.."
    REQUIRES
        esp_timer
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra)

# ESP-IDF 组件内定义（防 dllexport 语义）
target_compile_definitions(${COMPONENT_LIB} PUBLIC ZENITH_STATIC)
