cmake_minimum_required(VERSION 3.16)
project(zenith_consumer_smoke CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 关键：只走已安装的包，不 add_subdirectory 源仓库
find_package(Zenith CONFIG REQUIRED)

add_executable(consumer_static main.cpp)
target_link_libraries(consumer_static PRIVATE Zenith::zenith_core)

if (TARGET Zenith::zenith)
    add_executable(consumer_shared main.cpp)
    target_link_libraries(consumer_shared PRIVATE Zenith::zenith)
endif()
