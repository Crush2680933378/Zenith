# libFuzzer 目标（Clang 专用）。CI 里这样用：
#   cmake -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
#         -DZENITH_SANITIZE=address,undefined -DZENITH_BUILD_FUZZERS=ON \
#         -DZENITH_BUILD_TESTS=OFF -DZENITH_BUILD_EXAMPLES=OFF -DZENITH_BUILD_PLUGINS=OFF
#   cmake --build build-fuzz
#   ./build-fuzz/fuzz/fuzz_http_request -max_total_time=60 fuzz/corpus/http_request

if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(STATUS "Zenith fuzzers: 需要 Clang + libFuzzer，当前 ${CMAKE_CXX_COMPILER_ID} — 跳过")
    return()
endif()

set(ZENITH_FUZZ_TARGETS
    fuzz_http_request
    fuzz_http_response
    fuzz_ws_frame
    fuzz_json
    fuzz_audio_wav)

foreach(t IN LISTS ZENITH_FUZZ_TARGETS)
    add_executable(${t} ${t}.cpp)
    target_link_libraries(${t} PRIVATE zenith_core Threads::Threads)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    # fuzzer 运行时单独加；内存错误由 -DZENITH_SANITIZE 决定（CI 用 address,undefined）
    target_compile_options(${t} PRIVATE -fsanitize=fuzzer -g -O1)
    target_link_options(${t} PRIVATE -fsanitize=fuzzer)
    set_target_properties(${t} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/fuzz)
endforeach()

# 种子语料：每个目标一个目录（libFuzzer 会把新发现的好样本写回去）
foreach(t IN LISTS ZENITH_FUZZ_TARGETS)
    add_custom_target(${t}_corpus ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
                ${CMAKE_SOURCE_DIR}/fuzz/corpus/${t}
        COMMENT "corpus dir: fuzz/corpus/${t}")
endforeach()
