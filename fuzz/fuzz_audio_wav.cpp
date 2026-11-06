// fuzz_audio_wav —— 语音前处理的 WAV 解析/降混/重采样（M8.8）
//
// 攻击面：用户上传的音频。chunk 长度字段全是攻击者可控的，
// 越界读会直接崩在解码里。
#include <cstddef>
#include <cstdint>
#include <string>

#include "zenith/ai/audio_pcm.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 256 * 1024) return 0;
    const std::string input(reinterpret_cast<const char*>(data), size);

    const auto info = zenith::ai::parseWavHeader(input);
    if (info.ok) {
        // 声明的数据区必须落在输入内（否则解码会越界）
        if (info.dataOffset > input.size()) __builtin_trap();
        if (info.dataOffset + info.dataBytes > input.size()) __builtin_trap();
        if (info.channels <= 0 || info.sampleRate <= 0) __builtin_trap();
    }

    const auto pcm = zenith::ai::decodeAudioTo16kMono(input, "wav");
    if (pcm.ok) {
        if (pcm.samples.empty()) __builtin_trap();
        for (float s : pcm.samples) {
            if (!(s >= -1.0001f && s <= 1.0001f)) __builtin_trap(); // 归一化越界
        }
    }
    // 裸 PCM 路径同样过一遍（格式判定不同）
    (void)zenith::ai::decodeAudioTo16kMono(input, "pcm16");
    return 0;
}
