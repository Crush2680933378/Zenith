# 基准服务端（性能门禁/本地对比用）；zenith_bench 本体在 tests/CMakeLists.txt 里
add_executable(zenith_bench_server bench_server.cpp)
target_link_libraries(zenith_bench_server PRIVATE zenith_core Threads::Threads)
target_include_directories(zenith_bench_server PRIVATE ${CMAKE_SOURCE_DIR}/include)
zenith_set_warnings(zenith_bench_server)
