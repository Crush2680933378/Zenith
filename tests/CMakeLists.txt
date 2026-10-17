# 单元测试（ZTEST 自研微框架）—— 链接静态库 zenith_core，逐可执行注册 ctest

set(ZENITH_TESTS
    test_core
    test_concurrency
    test_log
    test_plugin
    test_json
    test_http_parser
    test_router
    test_net
    test_http_e2e
    test_websocket
    test_redis
    test_config)

foreach(t IN LISTS ZENITH_TESTS)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE zenith_core Threads::Threads)
    target_include_directories(${t} PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/examples/common
        ${CMAKE_SOURCE_DIR}/include)
    zenith_set_warnings(${t})
    add_test(NAME ${t} COMMAND ${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 180)
endforeach()

# db 测试：仅当 uORM 集成（sqlite 驱动可用性由 uORM 自身探测）
get_property(ZENITH_HAS_DB_PROP GLOBAL PROPERTY ZENITH_HAS_DB)
if (ZENITH_HAS_DB_PROP)
    add_executable(test_db test_db.cpp)
    target_link_libraries(test_db PRIVATE zenith_core Threads::Threads uORM::uorm)
    target_include_directories(test_db PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_db)
    add_test(NAME test_db COMMAND test_db)
    set_tests_properties(test_db PROPERTIES TIMEOUT 120)
endif()


# TLS 测试：仅当 OpenSSL 后端编入
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_tls test_tls.cpp)
    target_link_libraries(test_tls PRIVATE zenith_core Threads::Threads)
    target_compile_definitions(test_tls PRIVATE ZENITH_HAS_TLS=1)
    target_include_directories(test_tls PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_tls)
    add_test(NAME test_tls COMMAND test_tls)
    set_tests_properties(test_tls PROPERTIES TIMEOUT 180)
endif()

# M5.6 运维能力（热重载/管理台 REST；DB 部分需 uORM）
add_executable(test_m5_ops test_m5_ops.cpp)
if (TARGET uORM::uorm)
    target_link_libraries(test_m5_ops PRIVATE zenith_core Threads::Threads ws2_32 uORM::uorm)
else()
    target_link_libraries(test_m5_ops PRIVATE zenith_core Threads::Threads ws2_32)
    target_compile_definitions(test_m5_ops PRIVATE ZENITH_NO_DB=1)
endif()
target_include_directories(test_m5_ops PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m5_ops)
add_test(NAME test_m5_ops COMMAND test_m5_ops)
set_tests_properties(test_m5_ops PROPERTIES TIMEOUT 120)

# zenith-bench 压测工具（ZENITH_BUILD_BENCH，默认 ON）
if (ZENITH_BUILD_BENCH)
    add_executable(zenith_bench ../tools/zenith_bench.cpp)
    target_link_libraries(zenith_bench PRIVATE zenith_core Threads::Threads ws2_32)
    target_include_directories(zenith_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
    zenith_set_warnings(zenith_bench)
endif()

# M5.2 安全（SHA-256/HMAC/JWT/CORS/限流/安全头；需 TLS=openssl 以做 OpenSSL 交叉核对）
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_m5_sec test_m5_sec.cpp)
    target_link_libraries(test_m5_sec PRIVATE zenith_core Threads::Threads ws2_32)
    target_include_directories(test_m5_sec PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_m5_sec)
    add_test(NAME test_m5_sec COMMAND test_m5_sec)
    set_tests_properties(test_m5_sec PROPERTIES TIMEOUT 120)
endif()

# M5.3 可观测性（指标/Request-Id/status）
add_executable(test_m5_obs test_m5_obs.cpp)
target_link_libraries(test_m5_obs PRIVATE zenith_core Threads::Threads ws2_32)
target_include_directories(test_m5_obs PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m5_obs)
add_test(NAME test_m5_obs COMMAND test_m5_obs)
set_tests_properties(test_m5_obs PROPERTIES TIMEOUT 120)

# M5 Web 完整性（multipart/静态/session/优雅停机/wss；wss 需 TLS 后端）
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_m5_web test_m5_web.cpp)
    target_link_libraries(test_m5_web PRIVATE zenith_core Threads::Threads ws2_32)
    target_include_directories(test_m5_web PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_m5_web)
    add_test(NAME test_m5_web COMMAND test_m5_web)
    set_tests_properties(test_m5_web PROPERTIES TIMEOUT 120)
endif()

# ai 插件测试（mock ComfyUI 走本仓 HttpServer）
add_executable(test_ai test_ai.cpp)
target_link_libraries(test_ai PRIVATE zenith_core Threads::Threads ws2_32)
target_include_directories(test_ai PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/examples/common)
target_compile_definitions(test_ai PRIVATE
    ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
zenith_set_warnings(test_ai)
add_test(NAME test_ai COMMAND test_ai)
set_tests_properties(test_ai PROPERTIES TIMEOUT 120)

# edge 插件测试（cobs/serial 编入核心库）
add_executable(test_edge test_edge.cpp)
target_link_libraries(test_edge PRIVATE zenith_core Threads::Threads ws2_32)
target_include_directories(test_edge PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/examples/common)
target_compile_definitions(test_edge PRIVATE
    ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
zenith_set_warnings(test_edge)
add_test(NAME test_edge COMMAND test_edge)
set_tests_properties(test_edge PROPERTIES TIMEOUT 120)

# media 插件测试：仅当 media_ffmpeg 目标存在（FFmpeg SDK 找到）
if (TARGET zenith_media_ffmpeg)
    add_executable(test_media test_media.cpp)
    target_link_libraries(test_media PRIVATE zenith_core Threads::Threads)
    target_include_directories(test_media PRIVATE
        ${CMAKE_SOURCE_DIR}/tests
        ${CMAKE_SOURCE_DIR}/examples/common)
    target_compile_definitions(test_media PRIVATE
        ZENITH_HAS_MEDIA=1
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
    zenith_set_warnings(test_media)
    add_test(NAME test_media COMMAND test_media)
    set_tests_properties(test_media PROPERTIES TIMEOUT 300)
endif()

# 端到端插件测试需要示例插件已构建
if (ZENITH_BUILD_PLUGINS)
    target_compile_definitions(test_plugin PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
else()
    target_compile_definitions(test_plugin PRIVATE ZENITH_NO_PLUGIN_E2E)
endif()

# 可选协程层冒烟测试（C++20）
if (ZENITH_ENABLE_COROUTINES)
    add_executable(test_coro test_coro.cpp)
    target_link_libraries(test_coro PRIVATE zenith_coro zenith_core)
    target_include_directories(test_coro PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_features(test_coro PRIVATE cxx_std_20)
    zenith_set_warnings(test_coro)
    add_test(NAME test_coro COMMAND test_coro)
    set_tests_properties(test_coro PROPERTIES TIMEOUT 60)

    add_executable(test_coro_http test_coro_http.cpp)
    target_link_libraries(test_coro_http PRIVATE zenith_coro zenith_core Threads::Threads)
    target_include_directories(test_coro_http PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_features(test_coro_http PRIVATE cxx_std_20)
    zenith_set_warnings(test_coro_http)
    add_test(NAME test_coro_http COMMAND test_coro_http)
    set_tests_properties(test_coro_http PROPERTIES TIMEOUT 120)
endif()
