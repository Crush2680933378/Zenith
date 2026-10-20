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

# 链接导入库。
# ⚠️ Windows 下导入库有两种格式，选错直接「配置成功、链接必败」：
#   - MSVC/官方 shared SDK：<root>/lib/avformat.lib
#   - MSYS2 MinGW-w64：    <root>/lib/libavformat.dll.a
# 教训（Blog 实战，test_media 在 mingw64 环境永跑不起来的根因）：
# 不能写死 .lib——按「每库逐个探测哪种格式存在」选择，两者都缺则让插件
# 优雅降级（return 前置），而不是留下一个链接必败的目标拖垮 ctest。
if (WIN32)
    set(_FF_LIBS
        avformat avcodec avutil swscale swresample avfilter avdevice)
else()
    set(_FF_LIBS
        avformat avcodec avutil swscale swresample)
endif()
target_link_directories(zenith_media_ffmpeg PRIVATE ${_FF_ROOT}/lib)

if (WIN32)
    foreach(_l ${_FF_LIBS})
        if (EXISTS "${_FF_ROOT}/lib/${_l}.lib")
            target_link_libraries(zenith_media_ffmpeg PRIVATE "${_FF_ROOT}/lib/${_l}.lib")
        elseif (EXISTS "${_FF_ROOT}/lib/lib${_l}.dll.a")
            target_link_libraries(zenith_media_ffmpeg PRIVATE "${_FF_ROOT}/lib/lib${_l}.dll.a")
        elseif (EXISTS "${_FF_ROOT}/bin/${_l}.dll" OR EXISTS "${_FF_ROOT}/bin/${_l}-*.dll")
            # 仅运行时 DLL（无导入库）：让链接器直接吃 DLL（MinGW 支持）
            target_link_libraries(zenith_media_ffmpeg PRIVATE "${_FF_ROOT}/bin/${_l}.dll")
        else()
            message(WARNING "media_ffmpeg: 找不到 ${_l} 的导入库（.lib/.dll.a 均缺），链接可能失败——"
                            "建议设置 ZENITH_FFMPEG_ROOT 指向完整 SDK")
            target_link_libraries(zenith_media_ffmpeg PRIVATE ${_l})
        endif()
    endforeach()
else()
    foreach(_l ${_FF_LIBS})
        target_link_libraries(zenith_media_ffmpeg PRIVATE -l${_l})
    endforeach()
endif()

# 运行期需要 FFmpeg DLL 可达（bin 在 PATH 或拷贝到输出目录）
add_custom_command(TARGET zenith_media_ffmpeg POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:zenith_media_ffmpeg>"
        "${ZENITH_PLUGIN_OUTPUT_DIR}/$<TARGET_FILE_NAME:zenith_media_ffmpeg>")

zenith_set_warnings(zenith_media_ffmpeg)
