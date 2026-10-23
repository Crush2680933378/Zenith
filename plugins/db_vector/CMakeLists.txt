# db_vector —— 向量存储插件（自研 HNSW；内联编译 hnsw.cpp 同 ZenITH_STATIC 规则）
add_library(zenith_db_vector MODULE db_vector.cpp)

set_target_properties(zenith_db_vector PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_sources(zenith_db_vector PRIVATE ${CMAKE_SOURCE_DIR}/src/vector/hnsw.cpp)

target_include_directories(zenith_db_vector PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src)

target_compile_definitions(zenith_db_vector PRIVATE ZENITH_STATIC)

zenith_set_warnings(zenith_db_vector)
