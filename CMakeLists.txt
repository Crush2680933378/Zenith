# =============================================================================
#  Zenith —— 高并发 C++ 后端框架（纯 C ABI 插件内核）
#  最低要求：CMake 3.16 / C++17 / C11
# =============================================================================
cmake_minimum_required(VERSION 3.16)

project(zenith
    VERSION 0.1.0
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

set(ZENITH_TLS "none" CACHE STRING
    "TLS backend behind ITLSTransport: openssl | mbedtls | wolfssl | none (impl lands in M2/M3)")

# ---------------------------------------------------------------------------
# 全局编译设置
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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

# ---------------------------------------------------------------------------
# uORM 集成（M1：仅打通编译链接；M3：服务化封装 zenith::db 层）
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
        message(STATUS "Zenith: uORM integrated -> target uORM::uorm (MySQL/PgSQL/SQLite 按环境自动探测)")
    else()
        message(WARNING
            "ZENITH_ENABLE_ORM=ON 但缺少 thirdparty/uORM；"
            "执行: git submodule update --init --recursive")
    endif()
endif()

message(STATUS "Zenith ${PROJECT_VERSION}: "
               "static=${ZENITH_BUILD_STATIC} shared=${ZENITH_BUILD_SHARED} "
               "coro=${ZENITH_ENABLE_COROUTINES} orm=${ZENITH_ENABLE_ORM} tls=${ZENITH_TLS}")
