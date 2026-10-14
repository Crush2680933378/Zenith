# zenith 核心库目标：zenith_core(静态) + zenith(动态) + zenith_coro(可选接口层)

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
    http/http_client.cpp
    net/websocket.cpp
    redis/redis_client.cpp
    config/json_config.cpp
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
    target_compile_definitions(zenith_core PUBLIC ZENITH_STATIC)
    target_link_libraries(zenith_core PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS})
    set_target_properties(zenith_core PROPERTIES OUTPUT_NAME zenith)
endif()

# ---------------------------------------------------------------------------
# zenith —— 动态库（默认部署形态；符号经 ZENITH_API 显式导出）
# ---------------------------------------------------------------------------
if (ZENITH_BUILD_SHARED)
    add_library(zenith SHARED ${ZENITH_SOURCES} ${ZENITH_PUBLIC_HEADERS})
    zenith_target_include_public(zenith)
    zenith_set_warnings(zenith)
    target_compile_definitions(zenith PRIVATE ZENITH_BUILDING_LIBRARY)
    target_link_libraries(zenith PUBLIC Threads::Threads ${ZENITH_PLATFORM_LIBS} ${ZENITH_DB_LIBS})
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
