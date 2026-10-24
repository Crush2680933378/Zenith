# 单元测试（ZTEST 自研微框架）—— 链接静态库 zenith_core，逐可执行注册 ctest

if(WIN32)
    set(ZT_NETLIB ws2_32)
else()
    set(ZT_NETLIB)
endif()

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
    test_config
    test_stream
    test_compress)

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
    target_link_libraries(test_m5_ops PRIVATE zenith_core Threads::Threads ${ZT_NETLIB} uORM::uorm)
else()
    target_link_libraries(test_m5_ops PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_compile_definitions(test_m5_ops PRIVATE ZENITH_NO_DB=1)
endif()
target_include_directories(test_m5_ops PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m5_ops)
add_test(NAME test_m5_ops COMMAND test_m5_ops)
set_tests_properties(test_m5_ops PROPERTIES TIMEOUT 120)

# M6.4 协议补强（连接上限/空闲踢除/Expect-100）
add_executable(test_m6 test_m6.cpp)
target_link_libraries(test_m6 PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m6 PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m6)
add_test(NAME test_m6 COMMAND test_m6)
set_tests_properties(test_m6 PROPERTIES TIMEOUT 120)

# M10 AI Agent 后端（HNSW/MCP/Agent/Qdrant API）
add_executable(test_m10 test_m10.cpp)
target_link_libraries(test_m10 PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m10 PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m10)
add_test(NAME test_m10 COMMAND test_m10)
set_tests_properties(test_m10 PROPERTIES TIMEOUT 180)

# M11 多 Agent 集群（crew 全流程/投票/缺陷环/门禁/REST+SSE）
add_executable(test_m11 test_m11.cpp)
target_link_libraries(test_m11 PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m11 PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m11)
add_test(NAME test_m11 COMMAND test_m11)
set_tests_properties(test_m11 PROPERTIES TIMEOUT 180)

# M12 多厂商 LLM 路由（3 协议 mock 厂商）
add_executable(test_m12 test_m12.cpp)
target_link_libraries(test_m12 PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m12 PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m12)
add_test(NAME test_m12 COMMAND test_m12)
set_tests_properties(test_m12 PROPERTIES TIMEOUT 120)

# M13 第一批（TLS 路由/向量持久化/流式）
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_m13a test_m13a.cpp)
    target_link_libraries(test_m13a PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_m13a PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_m13a PRIVATE ZENITH_TLS_OPENSSL=1)
    zenith_set_warnings(test_m13a)
    add_test(NAME test_m13a COMMAND test_m13a)
    set_tests_properties(test_m13a PROPERTIES TIMEOUT 120)
endif()

# M13 第二批（failover/熔断/成本 + ai_guard + 语音 mock）
add_executable(test_m13b test_m13b.cpp)
target_link_libraries(test_m13b PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m13b PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m13b)
add_test(NAME test_m13b COMMAND test_m13b)
set_tests_properties(test_m13b PROPERTIES TIMEOUT 120)

# M13 第四批（cron/otel/oauth/mqtt）
add_executable(test_m13c test_m13c.cpp)
target_link_libraries(test_m13c PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m13c PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m13c)
add_test(NAME test_m13c COMMAND test_m13c)
set_tests_properties(test_m13c PROPERTIES TIMEOUT 180)

# M14：ai_crew 全面强化（取消/成本/持久化/记忆/辩论/子 crew/…）
add_executable(test_m13d test_m13d.cpp)
target_link_libraries(test_m13d PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m13d PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m13d)
add_test(NAME test_m13d COMMAND test_m13d)
set_tests_properties(test_m13d PROPERTIES TIMEOUT 300)

# M15：ZenithDesk 契约（roster/meta/聚合board/新事件/CORS/WS）
add_executable(test_m13e test_m13e.cpp)
target_link_libraries(test_m13e PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m13e PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m13e)
add_test(NAME test_m13e COMMAND test_m13e)
set_tests_properties(test_m13e PROPERTIES TIMEOUT 180)

# zenith-bench 压测工具（ZENITH_BUILD_BENCH，默认 ON）
if (ZENITH_BUILD_BENCH)
    add_executable(zenith_bench ../tools/zenith_bench.cpp)
    target_link_libraries(zenith_bench PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(zenith_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
    zenith_set_warnings(zenith_bench)
endif()

# M5.2 安全（SHA-256/HMAC/JWT/CORS/限流/安全头；需 TLS=openssl 以做 OpenSSL 交叉核对）
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_m5_sec test_m5_sec.cpp)
    target_link_libraries(test_m5_sec PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_m5_sec PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_m5_sec)
    add_test(NAME test_m5_sec COMMAND test_m5_sec)
    set_tests_properties(test_m5_sec PROPERTIES TIMEOUT 120)

    # M9.2 非对称 JWT（RS256/ES256，OpenSSL 后端）
    add_executable(test_jwt_asym test_jwt_asym.cpp)
    target_link_libraries(test_jwt_asym PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_jwt_asym PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_jwt_asym)
    add_test(NAME test_jwt_asym COMMAND test_jwt_asym)
    set_tests_properties(test_jwt_asym PROPERTIES TIMEOUT 120)
endif()

# M5.3 可观测性（指标/Request-Id/status）
add_executable(test_m5_obs test_m5_obs.cpp)
target_link_libraries(test_m5_obs PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_m5_obs PRIVATE ${CMAKE_SOURCE_DIR}/tests)
zenith_set_warnings(test_m5_obs)
add_test(NAME test_m5_obs COMMAND test_m5_obs)
set_tests_properties(test_m5_obs PROPERTIES TIMEOUT 120)

# M5 Web 完整性（multipart/静态/session/优雅停机/wss；wss 需 TLS 后端）
get_property(ZENITH_HAS_TLS_PROP GLOBAL PROPERTY ZENITH_HAS_TLS)
if (ZENITH_HAS_TLS_PROP)
    add_executable(test_m5_web test_m5_web.cpp)
    target_link_libraries(test_m5_web PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_m5_web PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    zenith_set_warnings(test_m5_web)
    add_test(NAME test_m5_web COMMAND test_m5_web)
    set_tests_properties(test_m5_web PROPERTIES TIMEOUT 120)
endif()

# ai 插件测试（mock ComfyUI 走本仓 HttpServer）
add_executable(test_ai test_ai.cpp)
target_link_libraries(test_ai PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
target_include_directories(test_ai PRIVATE
    ${CMAKE_SOURCE_DIR}/tests
    ${CMAKE_SOURCE_DIR}/examples/common)
target_compile_definitions(test_ai PRIVATE
    ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
zenith_set_warnings(test_ai)
add_test(NAME test_ai COMMAND test_ai)
set_tests_properties(test_ai PROPERTIES TIMEOUT 120)

# M8.1 ai_inference 插件（需插件已构建：mock 后端 + OpenAI 兼容层全链路）
if (ZENITH_BUILD_PLUGINS)
    add_executable(test_ai_inference test_ai_inference.cpp)
    target_link_libraries(test_ai_inference PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_ai_inference PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_ai_inference PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
    zenith_set_warnings(test_ai_inference)
    add_test(NAME test_ai_inference COMMAND test_ai_inference)
    set_tests_properties(test_ai_inference PROPERTIES TIMEOUT 180)
endif()

# M8.2 插件热重载（.new 交换协议；用 ai_inference 的 mock 后端，零成本）
if (ZENITH_BUILD_PLUGINS)
    add_executable(test_plugin_reload test_plugin_reload.cpp)
    target_link_libraries(test_plugin_reload PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_plugin_reload PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_plugin_reload PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
    zenith_set_warnings(test_plugin_reload)
    add_test(NAME test_plugin_reload COMMAND test_plugin_reload)
    set_tests_properties(test_plugin_reload PROPERTIES TIMEOUT 120)
endif()
# M8.1 两个引擎插件（都需要插件已构建；模型/引擎缺失时用例自行 SKIP）
if (ZENITH_BUILD_PLUGINS)
    add_executable(test_ai_llama test_ai_llama.cpp)
    target_link_libraries(test_ai_llama PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_ai_llama PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_ai_llama PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
    zenith_set_warnings(test_ai_llama)
    add_test(NAME test_ai_llama COMMAND test_ai_llama)
    set_tests_properties(test_ai_llama PROPERTIES TIMEOUT 900)

    add_executable(test_ai_ninfer_native test_ai_ninfer_native.cpp)
    target_link_libraries(test_ai_ninfer_native PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_ai_ninfer_native PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_ai_ninfer_native PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${CMAKE_BINARY_DIR}/plugins-native")
    zenith_set_warnings(test_ai_ninfer_native)
    add_test(NAME test_ai_ninfer_native COMMAND test_ai_ninfer_native)
    set_tests_properties(test_ai_ninfer_native PROPERTIES TIMEOUT 900)
    add_executable(test_ai_ninfer test_ai_ninfer.cpp)
    target_link_libraries(test_ai_ninfer PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
    target_include_directories(test_ai_ninfer PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_compile_definitions(test_ai_ninfer PRIVATE
        ZENITH_TEST_PLUGIN_DIR="${ZENITH_PLUGIN_OUTPUT_DIR}")
    zenith_set_warnings(test_ai_ninfer)
    add_test(NAME test_ai_ninfer COMMAND test_ai_ninfer)
    set_tests_properties(test_ai_ninfer PROPERTIES TIMEOUT 900)
endif()
# edge 插件测试（cobs/serial 编入核心库）
add_executable(test_edge test_edge.cpp)
target_link_libraries(test_edge PRIVATE zenith_core Threads::Threads ${ZT_NETLIB})
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
