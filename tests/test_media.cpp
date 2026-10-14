// media_ffmpeg 插件端到端测试（ZENITH_HAS_MEDIA 时编译）
// 流程：ffmpeg testsrc 生成 2 秒测试视频 → 宿主加载插件 → probe/snapshot/transcode → 验证产物
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <thread>

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

#include "media_service.hpp"

using namespace zenith;

#ifndef ZENITH_HAS_MEDIA
ZTEST(Media, SkippedNoSdk) { ZEXPECT_TRUE(true); }
ZTEST_MAIN
#else

namespace {
/// 调 ffmpeg CLI 生成测试素材（testsrc 2 秒 320x240 + 正弦音）
bool genTestVideo(const std::string& out) {
    std::error_code ec;
    std::filesystem::remove(out, ec);
    const std::string cmd =
        "ffmpeg -y -f lavfi -i testsrc=duration=2:size=320x240:rate=10 "
        "-f lavfi -i sine=frequency=440:duration=2 "
        "-c:v libx264 -pix_fmt yuv420p -c:a aac -shortest \"" +
        out + "\" 2>nul";
    const int rc = std::system(cmd.c_str());
    return rc == 0 && std::filesystem::exists(out);
}
} // namespace

ZTEST(MediaPlugin, LoadProbeSnapshotTranscode) {
    const std::string dir = "zmedia.tmp";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    const std::string src = dir + "/in.mp4";
    const std::string jpg = dir + "/frame.jpg";
    const std::string dst = dir + "/out.mp4";

    if (!genTestVideo(src)) {
        ztest::reportFail(__FILE__, __LINE__, "ffmpeg CLI 生成测试视频失败（PATH 需含 ffmpeg）");
        return;
    }

    // 宿主加载插件（与 hello_* 同目录；线程池必配——hello_cpp 的 init 依赖 post_task）
    concurrency::ThreadPool pool(2);
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, nullptr);
    auto loaded = mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);
    ZEXPECT_TRUE(loaded.value() >= 3); // hello_cpp + hello_c + media_ffmpeg

    void* raw = registry.queryRaw(media::IMediaService::service_name(),
                                   media::IMediaService::service_version());
    ZEXPECT_TRUE(raw != nullptr);
    if (raw == nullptr) return;
    auto* svc = static_cast<media::IMediaService*>(raw);

    // ---- probe ----
    media::MediaInfo info;
    ZEXPECT_TRUE(svc->probe(src, info));
    ZEXPECT_EQ(info.width, 320);
    ZEXPECT_EQ(info.height, 240);
    ZEXPECT_TRUE(info.durationSec > 1.5 && info.durationSec < 2.5);
    ZEXPECT_FALSE(info.vcodec.empty());
    ZEXPECT_EQ(info.acodec, std::string("aac"));
    ZEXPECT_TRUE(info.fps > 5);

    // ---- snapshot（1 秒处，缩放 160x120）----
    ZEXPECT_TRUE(svc->snapshot(src, 1.0, jpg, 160, 120));
    ZEXPECT_TRUE(std::filesystem::exists(jpg));
    ZEXPECT_TRUE(std::filesystem::file_size(jpg) > 1000);
    // JPEG 魔数
    {
        FILE* f = std::fopen(jpg.c_str(), "rb");
        if (f != nullptr) {
            unsigned char magic[2] = {0, 0};
            std::fread(magic, 1, 2, f);
            std::fclose(f);
            ZEXPECT_EQ(magic[0], 0xFF);
            ZEXPECT_EQ(magic[1], 0xD8);
        }
    }

    // ---- transcode（缩到 160x120，进度回调）----
    std::atomic<double> lastProgress{0};
    ZEXPECT_TRUE(svc->transcode(src, dst,
                                 media::TranscodeOptions{"libx264", "copy", 28, 160, 120, true, ""},
                                 [&](double p) { lastProgress.store(p); }));
    ZEXPECT_TRUE(std::filesystem::exists(dst));
    ZEXPECT_TRUE(std::filesystem::file_size(dst) > 1000);
    ZEXPECT_TRUE(lastProgress.load() > 0.5); // 至少推进过半

    // 产物可再次 probe（结构合法）
    media::MediaInfo outInfo;
    ZEXPECT_TRUE(svc->probe(dst, outInfo));
    ZEXPECT_EQ(outInfo.width, 160);
    ZEXPECT_EQ(outInfo.height, 120);

    // 卸载（服务清理不崩溃）
    mgr.unloadAll();
    ZEXPECT_EQ(registry.size(), size_t{0});

    std::filesystem::remove_all(dir, ec);
}

ZTEST(MediaPlugin, ProbeMissingFileFails) {
    concurrency::ThreadPool pool(1);
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, nullptr);
    (void)mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);
    void* raw = registry.queryRaw(media::IMediaService::service_name(), 1);
    if (raw == nullptr) {
        ztest::reportFail(__FILE__, __LINE__, "media 插件未加载");
        return;
    }
    auto* svc = static_cast<media::IMediaService*>(raw);
    media::MediaInfo info;
    ZEXPECT_FALSE(svc->probe("definitely_missing.mp4", info));
}

ZTEST_MAIN
#endif
