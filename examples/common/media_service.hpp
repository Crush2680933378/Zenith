// ============================================================================
//  IMediaService —— 媒体服务共享接口（宿主与 media-ffmpeg 插件之间）
//  与 greeter.hpp 同理：示例/插件共同包含；第三方可依同契约写别的后端
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "zenith/plugin/service.hpp"

namespace zenith::media {

struct MediaInfo {
    std::string path;
    double durationSec = 0;
    int width = 0;
    int height = 0;
    std::string vcodec;    // 视频编码（空 = 无视频流）
    std::string acodec;    // 音频编码（空 = 无音频流）
    double fps = 0;
    int64_t bitrate = 0;
};

struct TranscodeOptions {
    std::string vcodec = "libx264";
    std::string acodec = "aac";
    int crf = 23;             // x264 质量（18~28）
    int scaleWidth = 0;       // 0 = 保持
    int scaleHeight = 0;
    bool faststart = true;    // mp4 moov 前置
    std::string extraArgs;    // 透传（分号分隔）
};

/// 进度回调（0~1）
using ProgressCallback = std::function<void(double)>;

class IMediaService : public zenith::plugin::IService {
public:
    ZENITH_SERVICE_DECL(IMediaService, "zenith.media.IMediaService", 1)

    /// 探测媒体信息（阻塞调用方线程——经线程池/独立线程调用为宜）
    virtual bool probe(const std::string& path, MediaInfo& out) = 0;

    /// 截取 timeSec 处一帧为 JPEG（可缩放到 w×h，0 保持）
    virtual bool snapshot(const std::string& input, double timeSec,
                          const std::string& outputJpg, int w = 0, int h = 0) = 0;

    /// 转码（progress 可空）
    virtual bool transcode(const std::string& input, const std::string& output,
                           const TranscodeOptions& opt, ProgressCallback progress = nullptr) = 0;
};

} // namespace zenith::media
