# 基准工具：服务端（perf-gate 用）+ 压测客户端 zenith-bench
add_executable(zenith_bench_server bench_server.cpp)
target_link_libraries(zenith_bench_server PRIVATE zenith_core Threads::Threads)
target_include_directories(zenith_bench_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
zenith_set_warnings(zenith_bench_server)

# 放在 benchmarks/ 而不是 tests/：perf-gate 关掉测试也要能构建它
add_executable(zenith_bench ../tools/zenith_bench.cpp)
target_link_libraries(zenith_bench PRIVATE zenith_core Threads::Threads)
target_include_directories(zenith_bench PRIVATE ${CMAKE_SOURCE_DIR}/include)
zenith_set_warnings(zenith_bench)
