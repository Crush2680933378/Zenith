# zenith 核心库目标：zenith_core(静态) + zenith(动态) + zenith_coro(可选接口层)

# ---- TLS 后端（ZENITH_TLS=openssl；先于源列表展开）----
set(ZENITH_TLS_LIBS)
set(ZENITH_TLS_SOURCES)
# 没编入任何 TLS 后端时提供「编译得过、运行期明确报错」的空实现
# （tls_stub.cpp 内部用 #if !ZENITH_TLS_OPENSSL 保护，openssl 构建下为空 TU）
list(APPEND ZENITH_TLS_SOURCES tls/tls_stub.cpp)
if (ZENITH_TLS STREQUAL "openssl")
    # 用 find_package 而不是手工 find_library：链接项会变成 imported target
    # （OpenSSL::SSL / OpenSSL::Crypto），导出给下游时不会带构建机的绝对路径。
    find_package(OpenSSL QUIET)
    if (OpenSSL_FOUND)
        list(APPEND ZENITH_TLS_SOURCES tls/tls_context.cpp http/jwt_asym.cpp)
        set(ZENITH_TLS_LIBS OpenSSL::SSL OpenSSL::Crypto)
        set(ZENITH_HAS_OPENSSL ON)
        set(ZENITH_TLS_DEFINE ZENITH_TLS_OPENSSL=1)
        # 头文件路径必须传播（macOS brew openssl 不在默认搜索路径；
        # Linux/MinGW 恰在默认路径所以此前未暴露）
        set(ZENITH_TLS_INCLUDES ${OPENSSL_INCLUDE_DIR})
        set_property(GLOBAL PROPERTY ZENITH_HAS_TLS ON)
        message(STATUS "Zenith TLS: OpenSSL backend enabled")
    else()
        message(WARNING "ZENITH_TLS=openssl 但未找到 OpenSSL 开发库；TLS 禁用")
    endif()
endif()

# ---- 响应压缩（zlib 后端，可选的 C ABI 依赖）----
set(ZENITH_ZLIB_LIBS)
set(ZENITH_HAS_ZLIB OFF)
find_package(ZLIB QUIET)
if (ZLIB_FOUND)
    set(ZENITH_ZLIB_LIBS ZLIB::ZLIB)
    set(ZENITH_HAS_ZLIB ON)
    message(STATUS "Zenith compress: zlib backend enabled (gzip)")
else()
    message(STATUS "Zenith compress: zlib not found -> identity only")
endif()
# 传给上层：安装导出的 ZenithConfig.cmake 要据此生成 find_dependency
set_property(GLOBAL PROPERTY ZENITH_HAS_ZLIB ${ZENITH_HAS_ZLIB})

set(ZENITH_SOURCES
    core/key_value_config.cpp
    concurrency/thread_pool.cpp
    log/logger.cpp
    log/console_logger.cpp
    log/async_file_logger.cpp
    plugin/service_registry.cpp
    plugin/dynlib.cpp
    plugin/host_api.cpp
    plugin/plugin_manager.cpp
    plugin/plugin_watcher.cpp
    net/net_detail.cpp
    net/socket.cpp
    net/poller.cpp
    net/poller_wsapoll.cpp
    net/poller_epoll.cpp
    net/poller_kqueue.cpp
    net/poller_io_uring.cpp
    net/event_loop.cpp
    net/event_loop_thread_pool.cpp
    mqtt/mqtt.cpp
    mqtt/mqtt_broker.cpp
    net/tcp_connection.cpp
    net/tcp_server.cpp
    net/tcp_client.cpp
    json/json.cpp
    http/http_common.cpp
    http/http_parser.cpp
    http/router.cpp
    http/http_server.cpp
    http/sse.cpp
    http/compress.cpp
    http/http_client.cpp
    http/multipart.cpp
    http/static_files.cpp
    http/session.cpp
    http/context.cpp
    net/websocket.cpp
    redis/redis_client.cpp
    config/json_config.cpp
    config/hot_reload.cpp
    metrics/metrics.cpp
    ai/openai_api.cpp
    ai/ninfer_c_api_client.cpp
    crypto/sha256.cpp
    http/jwt.cpp
    http/security.cpp
    net/signal.cpp
    net/worker_supervisor_posix.cpp
    io/cobs.cpp
    vector/hnsw.cpp
    vector/vector_persist.cpp
    agent/agent.cpp
    crew/crew.cpp
    ai/provider_router.cpp
    ai/guard_service.cpp
    ai/speech_mock.cpp
    ai/audio_pcm.cpp
    ai/whisper_stt.cpp
    obs/trace_context.cpp
    concurrency/cron.cpp
    http/otel.cpp
    http/http2.cpp
    http/grpc.cpp
    http/oauth.cpp
    net/mqtt.cpp
    mcp/mcp_client.cpp
    mcp/mcp_server.cpp
    io/serial_port.cpp
    ${ZENITH_TLS_SOURCES}
)

set(ZENITH_PUBLIC_HEADERS
    ${CMAKE_SOURCE_DIR}/include/zenith/abi/zenith_abi.h
)

if (WIN32)
    set(ZENITH_PLATFORM_LIBS ws2_32)
else()
    set(ZENITH_PLATFORM_LIBS)
endif()

# ---- db 层（依赖 uORM 子模块；ZENITH_ENABLE_ORM=ON 时编入） ----
if (TARGET uORM::uorm)
    list(APPEND ZENITH_SOURCES db/db_service.cpp)
    set(ZENITH_DB_LIBS uORM::uorm)
    set_property(GLOBAL PROPERTY ZENITH_HAS_DB ON)
else()
    set(ZENITH_DB_LIBS)
endif()

# ---- whisper.cpp（可选，M8.8）：-DZENITH_WHISPER_ROOT=<whisper.cpp 根目录> ----
# 不配置时 whisper_stt.cpp 编成「不可用占位」：工厂返回 nullptr + 原因，
# 插件自动回落 mock。这样默认构建零依赖，要用真 STT 才引入 whisper。
set(ZENITH_WHISPER_INCLUDES "")
set(ZENITH_WHISPER_LIBS "")
set(ZENITH_WHISPER_DEFINE "")
if (ZENITH_WHISPER_ROOT)
    find_path(ZENITH_WHISPER_INC whisper.h
        HINTS "${ZENITH_WHISPER_ROOT}" "${ZENITH_WHISPER_ROOT}/include"
              "${ZENITH_WHISPER_ROOT}/include/whisper" "${ZENITH_WHISPER_ROOT}/ggml/include")
    find_library(ZENITH_WHISPER_LIB NAMES whisper libwhisper
        HINTS "${ZENITH_WHISPER_ROOT}" "${ZENITH_WHISPER_ROOT}/build/src"
              "${ZENITH_WHISPER_ROOT}/build" "${ZENITH_WHISPER_ROOT}/lib")
    if (ZENITH_WHISPER_INC AND ZENITH_WHISPER_LIB)
        set(ZENITH_WHISPER_INCLUDES ${ZENITH_WHISPER_INC})
        set(ZENITH_WHISPER_LIBS ${ZENITH_WHISPER_LIB})
        set(ZENITH_WHISPER_DEFINE ZENITH_HAS_WHISPER=1)
        message(STATUS "Zenith whisper.cpp: ${ZENITH_WHISPER_LIB}")
    else()
        message(STATUS "Zenith whisper.cpp: 未在 ZENITH_WHISPER_ROOT 找到 whisper.h/libwhisper — "
                       "STT 编为不可用占位（插件回落 mock）")
    endif()
endif()

# ---------------------------------------------------------------------------
# zenith_core —— 静态库（嵌入式 / 单体部署形态）
# ---------------------------------------------------------------------------
if (ZENITH_BUILD_STATIC)
    add_library(zenith_core STATIC ${ZENITH_SOURCES} ${ZENITH_PUBLIC_HEADERS})
    zenith_target_include_public(zenith_core)
    zenith_set_warnings(zenith_core)
    target_compile_definitions(zenith_core PUBLIC ZENITH_STATIC ${ZENITH_TLS_DEFINE})
    if (ZENITH_TLS_INCLUDES)
        target_include_directories(zenith_core PUBLIC ${ZENITH_TLS_INCLUDES})
    endif()
    if (ZENITH_HAS_OPENSSL)
        target_compile_definitions(zenith_core PUBLIC ZENITH_HAS_OPENSSL=1)
    endif()
    if (ZENITH_HAS_ZLIB)
        target_compile_definitions(zenith_core PRIVATE ZENITH_HAS_ZLIB=1)
    endif()
    if (ZENITH_WHISPER_DEFINE)
        target_compile_definitions(zenith_core PRIVATE ${ZENITH_WHISPER_DEFINE})
        target_include_directories(zenith_core SYSTEM PRIVATE ${ZENITH_WHISPER_INCLUDES})
    endif()
    target_link_libraries(zenith_core PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS} ${ZENITH_TLS_LIBS} ${ZENITH_ZLIB_LIBS} ${ZENITH_WHISPER_LIBS})
    set_target_properties(zenith_core PROPERTIES OUTPUT_NAME zenith)
endif()

# ---------------------------------------------------------------------------
# zenith —— 动态库（默认部署形态；符号经 ZENITH_API 显式导出）
# ---------------------------------------------------------------------------
if (ZENITH_BUILD_SHARED)
    add_library(zenith SHARED ${ZENITH_SOURCES} ${ZENITH_PUBLIC_HEADERS})
    zenith_target_include_public(zenith)
    zenith_set_warnings(zenith)
    target_compile_definitions(zenith PRIVATE ZENITH_BUILDING_LIBRARY ${ZENITH_TLS_DEFINE})
    if (ZENITH_TLS_INCLUDES)
        target_include_directories(zenith PUBLIC ${ZENITH_TLS_INCLUDES})
    endif()
    if (ZENITH_HAS_OPENSSL)
        target_compile_definitions(zenith PUBLIC ZENITH_HAS_OPENSSL=1)
    endif()
    if (ZENITH_HAS_ZLIB)
        target_compile_definitions(zenith PRIVATE ZENITH_HAS_ZLIB=1)
    endif()
    if (ZENITH_WHISPER_DEFINE)
        target_compile_definitions(zenith PRIVATE ${ZENITH_WHISPER_DEFINE})
        target_include_directories(zenith SYSTEM PRIVATE ${ZENITH_WHISPER_INCLUDES})
    endif()
    target_link_libraries(zenith PRIVATE ${ZENITH_WHISPER_LIBS})
    target_link_libraries(zenith PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS} ${ZENITH_TLS_LIBS} ${ZENITH_ZLIB_LIBS})
    set_target_properties(zenith PROPERTIES
        OUTPUT_NAME zenith
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
    # MSVC 下动态库会产出导入库 zenith.lib，与静态库 zenith_core 的 zenith.lib
    # 同名同目录 → ninja 报 "multiple rules generate lib/zenith.lib"。
    # 把导入库放到 lib/import/ 子目录即可（CMake 消费方走 target，不受影响）。
    if (MSVC AND ZENITH_BUILD_STATIC)
        set_target_properties(zenith PROPERTIES
            ARCHIVE_OUTPUT_DIRECTORY ${ZENITH_LIBRARY_OUTPUT_DIR}/import)
    endif()
endif()

# ---------------------------------------------------------------------------
# zenith_coro —— 可选协程层（ZENITH_ENABLE_COROUTINES=ON，要求 C++20）
# 纯头文件接口层：消费者切换到 cxx_std_20，核心库保持 C++17 不受影响。
# ---------------------------------------------------------------------------
if (ZENITH_ENABLE_COROUTINES)
    add_library(zenith_coro INTERFACE)
    target_include_directories(zenith_coro INTERFACE
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_features(zenith_coro INTERFACE cxx_std_20)
endif()
