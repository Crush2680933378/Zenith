# Zenith 通用构建工具

# 统一警告等级（Zenith 自有代码一律零警告；C++ 专属旗标按语言过滤）
function(zenith_set_warnings target)
    if (MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow
            $<$<COMPILE_LANGUAGE:CXX>:-Wnon-virtual-dtor -Wold-style-cast>)
    endif()
endfunction()

# 对齐 Zenith 的公共 include 路径（公共头 + 纯 C ABI 头）
function(zenith_target_include_public target)
    target_include_directories(${target} PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/abi>
        $<INSTALL_INTERFACE:include>)
endfunction()
