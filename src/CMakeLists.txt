# zenith 核心库目标：zenith_core(静态) + zenith(动态) + zenith_coro(可选接口层)

# ---- TLS 后端（ZENITH_TLS=openssl；先于源列表展开）----
set(ZENITH_TLS_LIBS)
set(ZENITH_TLS_SOURCES)
if (ZENITH_TLS STREQUAL "openssl")
    find_path(OPENSSL_INCLUDE_DIR openssl/ssl.h)
    find_library(OPENSSL_SSL_LIB NAMES ssl libssl)
    find_library(OPENSSL_CRYPTO_LIB NAMES crypto libcrypto)
    if (OPENSSL_INCLUDE_DIR AND OPENSSL_SSL_LIB AND OPENSSL_CRYPTO_LIB)
        list(APPEND ZENITH_TLS_SOURCES tls/tls_context.cpp http/jwt_asym.cpp)
        set(ZENITH_TLS_LIBS ${OPENSSL_SSL_LIB} ${OPENSSL_CRYPTO_LIB})
        set(ZENITH_HAS_OPENSSL ON)
        set(ZENITH_TLS_DEFINE ZENITH_TLS_OPENSSL=1)
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
    net/event_loop.cpp
    net/event_loop_thread_pool.cpp
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

# ---------------------------------------------------------------------------
# zenith_core —— 静态库（嵌入式 / 单体部署形态）
# ---------------------------------------------------------------------------
if (ZENITH_BUILD_STATIC)
    add_library(zenith_core STATIC ${ZENITH_SOURCES} ${ZENITH_PUBLIC_HEADERS})
    zenith_target_include_public(zenith_core)
    zenith_set_warnings(zenith_core)
    target_compile_definitions(zenith_core PUBLIC ZENITH_STATIC ${ZENITH_TLS_DEFINE})
    if (ZENITH_HAS_OPENSSL)
        target_compile_definitions(zenith_core PUBLIC ZENITH_HAS_OPENSSL=1)
    endif()
    if (ZENITH_HAS_ZLIB)
        target_compile_definitions(zenith_core PRIVATE ZENITH_HAS_ZLIB=1)
    endif()
    target_link_libraries(zenith_core PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS} ${ZENITH_TLS_LIBS} ${ZENITH_ZLIB_LIBS})
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
    if (ZENITH_HAS_OPENSSL)
        target_compile_definitions(zenith PUBLIC ZENITH_HAS_OPENSSL=1)
    endif()
    if (ZENITH_HAS_ZLIB)
        target_compile_definitions(zenith PRIVATE ZENITH_HAS_ZLIB=1)
    endif()
    target_link_libraries(zenith PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS} ${ZENITH_TLS_LIBS} ${ZENITH_ZLIB_LIBS})
    set_target_properties(zenith PROPERTIES
        OUTPUT_NAME zenith
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON)
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
