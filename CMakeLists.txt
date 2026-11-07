# =============================================================================
#  Zenith —— 高并发 C++ 后端框架（纯 C ABI 插件内核）
#  最低要求：CMake 3.16 / C++17 / C11
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(zenith
    VERSION 0.2.4
    DESCRIPTION "Zenith: high-concurrency C++ backend framework with a pure C ABI plugin kernel"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# 选项
# ---------------------------------------------------------------------------
option(ZENITH_BUILD_STATIC        "Build zenith_core static library"            ON)
option(ZENITH_BUILD_SHARED        "Build zenith shared library"                 ON)
option(ZENITH_BUILD_TESTS         "Build unit tests (ZTEST micro framework)"    ON)
option(ZENITH_BUILD_EXAMPLES      "Build examples"                              ON)
option(ZENITH_BUILD_PLUGINS       "Build sample plugins"                        ON)
option(ZENITH_ENABLE_COROUTINES   "Enable optional C++20 coroutine layer (zenith_coro)" OFF)
option(ZENITH_ENABLE_ORM          "Integrate thirdparty/uORM (git submodule)"   ON)
option(ZENITH_BUILD_BENCH        "Build zenith-bench load test tool"            ON)
option(ZENITH_BUILD_FUZZERS      "Build libFuzzer targets（需要 Clang + libFuzzer）"   OFF)
option(ZENITH_COVERAGE           "启用覆盖率插桩（Clang 的 -fprofile-instr-generate / GCC 的 --coverage）" OFF)

set(ZENITH_TLS "none" CACHE STRING
    "TLS backend behind ITLSTransport: openssl | mbedtls | wolfssl | none (impl lands in M2/M3)")

set(ZENITH_SANITIZE "" CACHE STRING
    "启用 sanitizer：address | thread | undefined，可组合（如 address,undefined）；空=关闭")

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
# 覆盖率插桩（CI 用）：Clang 走 profile-instr（llvm-profdata/llvm-cov），GCC 走 gcov
# ---------------------------------------------------------------------------
if (ZENITH_COVERAGE)
    if (CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(STATUS "Zenith coverage: llvm profile-instr-generate")
        add_compile_options(-fprofile-instr-generate -fcoverage-mapping -g -O1)
        add_link_options(-fprofile-instr-generate)
    elseif (CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(STATUS "Zenith coverage: gcov (--coverage)")
        add_compile_options(--coverage -g -O0)
        add_link_options(--coverage)
    else()
        message(WARNING "ZENITH_COVERAGE 被忽略：不支持的编译器 ${CMAKE_CXX_COMPILER_ID}")
    endif()
endif()

# ---------------------------------------------------------------------------
# sanitizer（CI 用；本地也可开）
#
#   -DZENITH_SANITIZE=address,undefined   内存错误 + UB（互斥于 thread）
#   -DZENITH_SANITIZE=thread              数据竞争（只能单开）
#
# 注意：ASan 与 TSan 不能同时启用，CI 里是两个独立构建目录。
# MinGW/MSVC 不支持这些 GCC/Clang 标志，这里显式忽略而不是让构建炸。
# ---------------------------------------------------------------------------
if (ZENITH_SANITIZE)
    if (MSVC OR (WIN32 AND NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang"))
        message(WARNING "ZENITH_SANITIZE=${ZENITH_SANITIZE} 被忽略：Windows 下 "
                        "MinGW/MSVC 对 -fsanitize 支持不完整（当前编译器 "
                        "${CMAKE_CXX_COMPILER_ID}）。请用 Linux/macOS + GCC/Clang 构建。")
    else()
        message(STATUS "Zenith sanitizer: ${ZENITH_SANITIZE}")
        add_compile_options(-fsanitize=${ZENITH_SANITIZE} -fno-omit-frame-pointer
                            -fno-sanitize-recover=all -g)
        add_link_options(-fsanitize=${ZENITH_SANITIZE})
        # UBSan 的 vptr 检查需要 RTTI；项目未关 RTTI，这里只是避免 -fno-rtti 场景踩坑
    endif()
endif()

# 输出布局：bin/ 放可执行与 DLL，lib/ 放静态库/so，plugins/ 放插件模块
set(ZENITH_RUNTIME_OUTPUT_DIR ${CMAKE_BINARY_DIR}/bin)
set(ZENITH_LIBRARY_OUTPUT_DIR ${CMAKE_BINARY_DIR}/lib)
set(ZENITH_PLUGIN_OUTPUT_DIR  ${CMAKE_BINARY_DIR}/plugins)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${ZENITH_RUNTIME_OUTPUT_DIR})
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${ZENITH_LIBRARY_OUTPUT_DIR})
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${ZENITH_LIBRARY_OUTPUT_DIR})

if (MSVC)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(ZenithUtils)

find_package(Threads REQUIRED)

if (ZENITH_BUILD_TESTS)
    enable_testing()
endif()

# ---------------------------------------------------------------------------
# uORM 集成（先于 src：db 层与测试需要 TARGET uORM::uorm 存在）
# 只取 uORM::uorm 核心目标；其 asio Web 控制台与示例全部关闭。
# ---------------------------------------------------------------------------
if (ZENITH_ENABLE_ORM)
    if (EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/uORM/CMakeLists.txt)
        set(BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
        set(UORM_BUILD_TESTS   OFF CACHE BOOL "" FORCE)
        set(UORM_ENABLE_WEB    OFF CACHE BOOL "" FORCE)
        set(UORM_BUILD_C_ABI   OFF CACHE BOOL "" FORCE)
        set(UORM_BUILD_UORMPP  OFF CACHE BOOL "" FORCE)
        add_subdirectory(thirdparty/uORM)
        # uORM 头以 SYSTEM 引入：其内部告警不进入 Zenith 的零警告基线
        if (TARGET uORM::uorm)
            get_target_property(_uorm_incs uorm INTERFACE_INCLUDE_DIRECTORIES)
            if (_uorm_incs)
                set_target_properties(uorm PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "")
                target_include_directories(uorm SYSTEM INTERFACE ${_uorm_incs})
            endif()
        endif()
        message(STATUS "Zenith: uORM integrated -> target uORM::uorm (MySQL/PgSQL/SQLite 按环境自动探测)")
    else()
        message(WARNING
            "ZENITH_ENABLE_ORM=ON 但缺少 thirdparty/uORM；"
            "执行: git submodule update --init --recursive")
    endif()
endif()

# ---------------------------------------------------------------------------
# 目标
# ---------------------------------------------------------------------------
add_subdirectory(src)

if (ZENITH_BUILD_PLUGINS)
    add_subdirectory(plugins)
endif()

if (ZENITH_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if (ZENITH_BUILD_TESTS)
    add_subdirectory(tests)
endif()

# libFuzzer 目标（Clang 专用；CI 的 fuzz job 用 -DZENITH_BUILD_FUZZERS=ON 打开）
if (ZENITH_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()

# 基准服务端（性能门禁用；zenith_bench 在 tests/ 下）
if (ZENITH_BUILD_BENCH)
    add_subdirectory(benchmarks)
endif()

# ---------------------------------------------------------------------------
# 安装与导出（M5.5）：find_package(Zenith CONFIG) 消费
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
install(DIRECTORY include/zenith DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(DIRECTORY abi/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/zenith/abi FILES_MATCHING PATTERN "*.h")

if (ZENITH_BUILD_STATIC)
    install(TARGETS zenith_core EXPORT ZenithTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    if (TARGET uorm)
        install(TARGETS uorm EXPORT ZenithTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
    if (TARGET ujson)
        install(TARGETS ujson EXPORT ZenithTargets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
    endif()
endif()
if (ZENITH_BUILD_SHARED)
    install(TARGETS zenith EXPORT ZenithTargets
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()

if (ZENITH_BUILD_STATIC OR ZENITH_BUILD_SHARED)
    install(EXPORT ZenithTargets
            NAMESPACE Zenith::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zenith)
    # 导出配置里的依赖开关（ZenithConfig.cmake.in 据此生成 find_dependency）
    get_property(_zenith_has_tls GLOBAL PROPERTY ZENITH_HAS_TLS)
    get_property(_zenith_has_zlib GLOBAL PROPERTY ZENITH_HAS_ZLIB)
    set(ZENITH_CONFIG_HAS_OPENSSL FALSE)
    set(ZENITH_CONFIG_HAS_ZLIB FALSE)
    if (_zenith_has_tls)
        set(ZENITH_CONFIG_HAS_OPENSSL TRUE)
    endif()
    if (_zenith_has_zlib)
        set(ZENITH_CONFIG_HAS_ZLIB TRUE)
    endif()
    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ZenithConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/ZenithConfig.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zenith)
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/ZenithConfigVersion.cmake
        VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/ZenithConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/ZenithConfigVersion.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Zenith)
    # 许可证与使用说明随包分发（下游/合规检查都要求）
    install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/LICENSE
                  ${CMAKE_CURRENT_SOURCE_DIR}/README.md
            DESTINATION ${CMAKE_INSTALL_DOCDIR})
endif()

message(STATUS "Zenith ${PROJECT_VERSION}: "
               "static=${ZENITH_BUILD_STATIC} shared=${ZENITH_BUILD_SHARED} "
               "coro=${ZENITH_ENABLE_COROUTINES} orm=${ZENITH_ENABLE_ORM} tls=${ZENITH_TLS}")
