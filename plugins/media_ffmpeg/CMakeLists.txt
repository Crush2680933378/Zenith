# media_ffmpeg —— FFmpeg 媒体插件（MODULE；依赖外部 FFmpeg shared SDK）
# SDK 定位：ZENITH_FFMPEG_ROOT 或自动探测 PATH 中的 ffmpeg（bin/include 布局）

set(_FF_ROOT "${ZENITH_FFMPEG_ROOT}")
if (NOT _FF_ROOT)
    # 从 PATH 里的 ffmpeg.exe 反推 SDK 根（Windows 布局 <root>/bin/ffmpeg.exe）
    find_program(_FF_EXE NAMES ffmpeg ffmpeg.exe)
    if (_FF_EXE)
        get_filename_component(_FF_BIN "${_FF_EXE}" DIRECTORY)
        get_filename_component(_FF_ROOT_CAND "${_FF_BIN}" DIRECTORY)
        if (EXISTS "${_FF_ROOT_CAND}/include/libavformat/avformat.h")
            set(_FF_ROOT "${_FF_ROOT_CAND}")
        endif()
    endif()
endif()

if (NOT _FF_ROOT OR NOT EXISTS "${_FF_ROOT}/include/libavformat/avformat.h")
    message(STATUS "Zenith media_ffmpeg: 未找到 FFmpeg SDK，插件跳过"
                   "（设置 ZENITH_FFMPEG_ROOT 启用）")
    return()
endif()

message(STATUS "Zenith media_ffmpeg: FFmpeg SDK at ${_FF_ROOT}")

add_library(zenith_media_ffmpeg MODULE media_ffmpeg.cpp)

set_target_properties(zenith_media_ffmpeg PROPERTIES
    PREFIX "" CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)

target_include_directories(zenith_media_ffmpeg PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/examples/common
    ${_FF_ROOT}/include)

# 链接导入库（Windows .lib；Linux 直接 -l）
if (WIN32)
    set(_FF_LIBS
        avformat avcodec avutil swscale swresample avfilter avdevice)
    target_link_directories(zenith_media_ffmpeg PRIVATE ${_FF_ROOT}/lib)
else()
    set(_FF_LIBS
        avformat avcodec avutil swscale swresample)
    target_link_directories(zenith_media_ffmpeg PRIVATE ${_FF_ROOT}/lib)
endif()
foreach(_l ${_FF_LIBS})
    if (WIN32)
        target_link_libraries(zenith_media_ffmpeg PRIVATE ${_FF_ROOT}/lib/${_l}.lib)
    else()
        target_link_libraries(zenith_media_ffmpeg PRIVATE -l${_l})
    endif()
endforeach()

# 运行期需要 FFmpeg DLL 可达（bin 在 PATH 或拷贝到输出目录）
add_custom_command(TARGET zenith_media_ffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:zenith_media_ffmpeg>"
        "${ZENITH_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:zenith_media_ffmpeg>")

zenith_set_warnings(zenith_media_ffmpeg)
