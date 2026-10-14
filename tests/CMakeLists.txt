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
    test_http_e2e)

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
