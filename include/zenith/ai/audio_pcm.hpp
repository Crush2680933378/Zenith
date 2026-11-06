// ============================================================================
//  zenith::ai::PcmAudio —— 语音前处理（M8.8）
//
//  语音后端（whisper.cpp）只接受 16kHz 单声道 float32 [-1,1]。
//  真实输入五花八门：浏览器录音是 44.1k/48k 立体声 WAV，电话录音是 8k 单声道，
//  这里统一做「解析 → 拆声道 → 静音降混 → 线性重采样」。
//
//  纯函数、零依赖：不依赖 whisper，任何构建下都有，便于单测与复用。
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zenith/core/export.hpp"

namespace zenith::ai {

/// WAV 头解析结果（只读元信息，不拷样本）
struct WavInfo {
    bool ok = false;
    int formatTag = 0;    // 1=PCM, 3=IEEE float, 0xFFFE=extensible（已解出子格式）
    int channels = 0;
    int sampleRate = 0;
    int bitsPerSample = 0;
    size_t dataOffset = 0; // 样本数据起始偏移
    size_t dataBytes = 0;
    std::string error;
};

/// 解析 RIFF/WAVE 头（chunk 顺序任意，跳过 LIST/fact 等）
ZENITH_API WavInfo parseWavHeader(std::string_view bytes);

/// 16kHz 单声道 float 样本（whisper 输入形态）
struct PcmAudio {
    bool ok = false;
    std::vector<float> samples;
    int sourceRate = 0;     // 原始采样率
    int sourceChannels = 0; // 原始声道数
    int durationMs = 0;     // 按 16kHz 输出长度折算的时长
    std::string error;
};

/// 把音频字节解码成 16kHz 单声道 float。
/// format:
///   "wav"   —— PCM 8/16/24/32bit 与 float32，任意采样率/声道
///   "pcm16" —— 裸 int16 小端，约定 16kHz 单声道（电话/流式常见）
/// 空 format 时按 RIFF 魔数自动判定。
ZENITH_API PcmAudio decodeAudioTo16kMono(std::string_view bytes, const std::string& format);

} // namespace zenith::ai
