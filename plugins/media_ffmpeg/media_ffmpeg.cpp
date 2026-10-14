// ============================================================================
//  media_ffmpeg —— C ABI 插件：FFmpeg 媒体服务（probe / snapshot / transcode）
//  依赖：FFmpeg shared SDK（C ABI）；由 CMake 变量 ZENITH_FFMPEG_ROOT 定位。
//  实现：avformat 探测 + 解码取帧（sws 缩放 → mjpeg）+ 转码管线
// ============================================================================
#include <cmath>
#include <cstdio>
#include <mutex>
#include <new>
#include <string>

#include "zenith/abi/zenith_abi.h"

#include "media_service.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace {

const zenith_host_api* g_host = nullptr;

[[maybe_unused]] std::string avErr(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_strerror(code, buf, sizeof(buf));
    return buf;
}
void logInfo(const std::string& msg) {
    if (g_host != nullptr) {
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "media_ffmpeg", 0, msg.c_str());
    }
}
void logError(const std::string& msg) {
    if (g_host != nullptr) {
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_ERROR, "media_ffmpeg", 0, msg.c_str());
    }
}

// ---------------------------------------------------------------------------
// FFmpeg 全局初始化（现代 API：avformat_network_init 可选）
// ---------------------------------------------------------------------------
struct AvGlobal {
    AvGlobal() { avformat_network_init(); }
};
const AvGlobal g_avGlobal;

class MediaServiceImpl final : public zenith::media::IMediaService {
public:
    bool probe(const std::string& path, zenith::media::MediaInfo& out) override {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) != 0 || fmt == nullptr) {
            logError("probe: open failed: " + path);
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            logError("probe: find_stream_info failed: " + path);
            return false;
        }
        out = {};
        out.path = path;
        out.durationSec = fmt->duration > 0 ? fmt->duration / static_cast<double>(AV_TIME_BASE)
                                            : 0.0;
        out.bitrate = fmt->bit_rate;

        const AVStream* vs = nullptr;
        const AVStream* as = nullptr;
        for (unsigned i = 0; i < fmt->nb_streams; ++i) {
            const AVStream* s = fmt->streams[i];
            if (s->codecpar == nullptr) continue;
            if (s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vs == nullptr) vs = s;
            if (s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && as == nullptr) as = s;
        }
        if (vs != nullptr && vs->codecpar != nullptr) {
            const AVCodecDescriptor* d =
                avcodec_descriptor_get(vs->codecpar->codec_id);
            out.vcodec = d != nullptr ? d->name : "";
            out.width = vs->codecpar->width;
            out.height = vs->codecpar->height;
            if (vs->avg_frame_rate.den != 0) {
                out.fps = av_q2d(vs->avg_frame_rate);
            }
        }
        if (as != nullptr && as->codecpar != nullptr) {
            const AVCodecDescriptor* d =
                avcodec_descriptor_get(as->codecpar->codec_id);
            out.acodec = d != nullptr ? d->name : "";
        }
        avformat_close_input(&fmt);
        return true;
    }

    bool snapshot(const std::string& input, double timeSec, const std::string& outputJpg,
                  int w, int h) override {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, input.c_str(), nullptr, nullptr) != 0) {
            logError("snapshot: open failed: " + input);
            return false;
        }
        if (avformat_find_stream_info(fmt, nullptr) < 0) {
            avformat_close_input(&fmt);
            return false;
        }
        int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vIdx < 0) {
            avformat_close_input(&fmt);
            logError("snapshot: no video stream: " + input);
            return false;
        }
        AVStream* vs = fmt->streams[vIdx];
        const AVCodec* dec = avcodec_find_decoder(vs->codecpar->codec_id);
        AVCodecContext* dc = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(dc, vs->codecpar);
        if (avcodec_open2(dc, dec, nullptr) < 0) {
            avcodec_free_context(&dc);
            avformat_close_input(&fmt);
            return false;
        }

        // 定位目标时间（seek 后必须 flush 解码器，否则返回陈旧帧）
        av_seek_frame(fmt, vIdx,
                       static_cast<int64_t>(timeSec / av_q2d(vs->time_base)),
                       AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(dc);

        AVPacket* pkt = av_packet_alloc();
        AVFrame* frame = av_frame_alloc();
        AVFrame* rgb = av_frame_alloc();
        bool gotFrame = false;
        while (av_read_frame(fmt, pkt) >= 0) {
            if (pkt->stream_index != vIdx) {
                av_packet_unref(pkt);
                continue;
            }
            if (avcodec_send_packet(dc, pkt) == 0) {
                while (avcodec_receive_frame(dc, frame) == 0) {
                    if (!gotFrame) {
                        gotFrame = true;
                        break;
                    }
                }
            }
            av_packet_unref(pkt);
            if (gotFrame) break;
        }

        bool ok = false;
        if (gotFrame) {
            const int ow = w > 0 ? w : frame->width;
            const int oh = h > 0 ? h : frame->height;
            // 弃用的 yuvj 全范围格式需映射为基础格式再做 sws（新 swscale 对 yuvj 断言）
            AVPixelFormat inFmt = static_cast<AVPixelFormat>(frame->format);
            if (inFmt == AV_PIX_FMT_YUVJ420P) inFmt = AV_PIX_FMT_YUV420P;
            SwsContext* sws =
                sws_getContext(frame->width, frame->height, inFmt, ow, oh,
                               AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (sws != nullptr) {
                av_image_alloc(rgb->data, rgb->linesize, ow, oh, AV_PIX_FMT_YUV420P, 1);
                sws_scale(sws, frame->data, frame->linesize, 0, frame->height, rgb->data,
                          rgb->linesize);

                // mjpeg 编码输出（JPEG 全范围色域；数据布局与 yuv420p 相同）
                const AVCodec* enc = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
                AVCodecContext* ec = avcodec_alloc_context3(enc);
                ec->width = ow;
                ec->height = oh;
                ec->pix_fmt = AV_PIX_FMT_YUVJ420P;
                ec->color_range = AVCOL_RANGE_JPEG;
                ec->time_base = {1, 25};
                if (avcodec_open2(ec, enc, nullptr) == 0) {
                    rgb->format = AV_PIX_FMT_YUVJ420P;
                    rgb->width = ow;
                    rgb->height = oh;
                    rgb->color_range = AVCOL_RANGE_JPEG;
                    rgb->pts = 0;
                    AVPacket* outPkt = av_packet_alloc();
                    if (avcodec_send_frame(ec, rgb) == 0 &&
                        avcodec_receive_packet(ec, outPkt) == 0) {
                        FILE* f = std::fopen(outputJpg.c_str(), "wb");
                        if (f != nullptr) {
                            std::fwrite(outPkt->data, 1, outPkt->size, f);
                            std::fclose(f);
                            ok = true;
                        }
                    }
                    av_packet_free(&outPkt);
                }
                avcodec_free_context(&ec);
                av_freep(&rgb->data[0]);
                sws_freeContext(sws);
            }
        }
        av_frame_free(&frame);
        av_frame_free(&rgb);
        av_packet_free(&pkt);
        avcodec_free_context(&dc);
        avformat_close_input(&fmt);
        if (!ok) logError("snapshot: failed: " + input);
        return ok;
    }

    bool transcode(const std::string& input, const std::string& output,
                   const zenith::media::TranscodeOptions& opt,
                   zenith::media::ProgressCallback progress) override {
        AVFormatContext* in = nullptr;
        if (avformat_open_input(&in, input.c_str(), nullptr, nullptr) != 0) {
            logError("transcode: open input failed: " + input);
            return false;
        }
        if (avformat_find_stream_info(in, nullptr) < 0) {
            avformat_close_input(&in);
            return false;
        }
        int vIdx = av_find_best_stream(in, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vIdx < 0) {
            avformat_close_input(&in);
            logError("transcode: no video stream");
            return false;
        }
        AVStream* ivs = in->streams[vIdx];

        // 输出
        AVFormatContext* out = nullptr;
        if (avformat_alloc_output_context2(&out, nullptr, nullptr, output.c_str()) < 0 ||
            out == nullptr) {
            avformat_close_input(&in);
            return false;
        }
        const AVCodec* encCodec = avcodec_find_encoder_by_name(opt.vcodec.c_str());
        if (encCodec == nullptr) encCodec = avcodec_find_encoder(AV_CODEC_ID_H264);
        if (encCodec == nullptr) {
            avformat_free_context(out);
            avformat_close_input(&in);
            logError("transcode: encoder not found: " + opt.vcodec);
            return false;
        }
        AVStream* ovs = avformat_new_stream(out, nullptr);
        AVCodecContext* ec = avcodec_alloc_context3(encCodec);
        ec->width = opt.scaleWidth > 0 ? opt.scaleWidth : ivs->codecpar->width;
        ec->height = opt.scaleHeight > 0 ? opt.scaleHeight : ivs->codecpar->height;
        ec->pix_fmt = AV_PIX_FMT_YUV420P;
        ec->time_base = ivs->avg_frame_rate.num > 0
                            ? AVRational{ivs->avg_frame_rate.den, ivs->avg_frame_rate.num}
                            : AVRational{1, 25};
        if (out->oformat->flags & AVFMT_GLOBALHEADER) {
            ec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        // 质量/预设
        if (opt.crf > 0) av_opt_set(ec->priv_data, "crf", std::to_string(opt.crf).c_str(), 0);
        av_opt_set(ec->priv_data, "preset", "veryfast", 0);
        if (avcodec_open2(ec, encCodec, nullptr) < 0) {
            avcodec_free_context(&ec);
            avformat_free_context(out);
            avformat_close_input(&in);
            return false;
        }
        avcodec_parameters_from_context(ovs->codecpar, ec);
        ovs->time_base = ec->time_base;

        if (!(out->oformat->flags & AVFMT_NOFILE)) {
            if (avio_open(&out->pb, output.c_str(), AVIO_FLAG_WRITE) < 0) {
                avcodec_free_context(&ec);
                avformat_free_context(out);
                avformat_close_input(&in);
                logError("transcode: open output failed: " + output);
                return false;
            }
        }
        if (avformat_write_header(out, nullptr) < 0) {
            avio_closep(&out->pb);
            avcodec_free_context(&ec);
            avformat_free_context(out);
            avformat_close_input(&in);
            return false;
        }

        // 解码器
        const AVCodec* decCodec = avcodec_find_decoder(ivs->codecpar->codec_id);
        AVCodecContext* dc = avcodec_alloc_context3(decCodec);
        avcodec_parameters_to_context(dc, ivs->codecpar);
        if (avcodec_open2(dc, decCodec, nullptr) < 0) {
            avcodec_free_context(&dc);
            avio_closep(&out->pb);
            avcodec_free_context(&ec);
            avformat_free_context(out);
            avformat_close_input(&in);
            return false;
        }

        // 帧转换（格式/尺寸 → yuv420p）
        SwsContext* sws = nullptr;
        AVFrame* frame = av_frame_alloc();
        AVFrame* cvt = av_frame_alloc();
        AVPacket* pkt = av_packet_alloc();
        AVPacket* outPkt = av_packet_alloc();
        const int64_t total = in->duration > 0 ? in->duration : 1;
        bool ok = true;
        int64_t outPts = 0;

        while (ok && av_read_frame(in, pkt) >= 0) {
            if (pkt->stream_index == vIdx && avcodec_send_packet(dc, pkt) == 0) {
                while (avcodec_receive_frame(dc, frame) == 0) {
                    if (sws == nullptr) {
                        AVPixelFormat inFmt = static_cast<AVPixelFormat>(frame->format);
                        if (inFmt == AV_PIX_FMT_YUVJ420P) inFmt = AV_PIX_FMT_YUV420P;
                        sws = sws_getContext(frame->width, frame->height, inFmt,
                                             ec->width, ec->height, AV_PIX_FMT_YUV420P,
                                             SWS_BILINEAR, nullptr, nullptr, nullptr);
                        av_image_alloc(cvt->data, cvt->linesize, ec->width, ec->height,
                                       AV_PIX_FMT_YUV420P, 1);
                        cvt->width = ec->width;
                        cvt->height = ec->height;
                        cvt->format = AV_PIX_FMT_YUV420P;
                    }
                    sws_scale(sws, frame->data, frame->linesize, 0, frame->height, cvt->data,
                              cvt->linesize);
                    cvt->pts = outPts++;
                    if (avcodec_send_frame(ec, cvt) == 0) {
                        while (avcodec_receive_packet(ec, outPkt) == 0) {
                            av_packet_rescale_ts(outPkt, dc->time_base, ec->time_base);
                            outPkt->stream_index = ovs->index;
                            if (av_interleaved_write_frame(out, outPkt) < 0) ok = false;
                        }
                    }
                    if (progress) {
                        progress(static_cast<double>(pkt->pts) * av_q2d(ivs->time_base) /
                                 (total / static_cast<double>(AV_TIME_BASE)));
                    }
                }
            }
            av_packet_unref(pkt);
        }
        // 冲刷编码器
        avcodec_send_frame(ec, nullptr);
        while (avcodec_receive_packet(ec, outPkt) == 0) {
            av_packet_rescale_ts(outPkt, dc->time_base, ec->time_base);
            outPkt->stream_index = ovs->index;
            av_interleaved_write_frame(out, outPkt);
            av_packet_unref(outPkt);
        }
        av_write_trailer(out);

        if (cvt->data[0] != nullptr) av_freep(&cvt->data[0]);
        av_frame_free(&frame);
        av_frame_free(&cvt);
        av_packet_free(&pkt);
        av_packet_free(&outPkt);
        if (sws != nullptr) sws_freeContext(sws);
        avcodec_free_context(&dc);
        avcodec_free_context(&ec);
        if (!(out->oformat->flags & AVFMT_NOFILE)) avio_closep(&out->pb);
        avformat_free_context(out);
        avformat_close_input(&in);
        if (ok) logInfo("transcode done: " + output);
        return ok;
    }
};

void destroyMediaService(void* p) {
    delete static_cast<MediaServiceImpl*>(p);
}

// ---------------------------------------------------------------------------
// 插件生命周期
// ---------------------------------------------------------------------------
zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    const std::string ver = "media_ffmpeg: init (libavformat " +
        std::to_string(LIBAVFORMAT_VERSION_MAJOR) + "." +
        std::to_string(LIBAVFORMAT_VERSION_MINOR) + ")";
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "media_ffmpeg", 0, ver.c_str());

    auto* svc = new (std::nothrow) MediaServiceImpl();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::media::IMediaService::service_name(),
                                  zenith::media::IMediaService::service_version(),
                                  static_cast<void*>(svc), &destroyMediaService);
}

zenith_status pluginStart(void) {
    logInfo("media_ffmpeg: start");
    return ZENITH_OK;
}

zenith_status pluginStop(void) {
    logInfo("media_ffmpeg: stop");
    return ZENITH_OK;
}

void pluginShutdown(void) {
    logInfo("media_ffmpeg: shutdown");
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "media_ffmpeg",
    "0.1.0",
    "Zenith media plugin: FFmpeg probe/snapshot/transcode",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
