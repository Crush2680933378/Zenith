// M8.8 语音前处理 + whisper 后端装配：
//   - WAV 头解析 / 位深转换 / 降混 / 重采样（whisper 要求的 16kHz 单声道 float）
//   - 未编入 whisper 时的降级语义（工厂返回 nullptr + 明确原因，不崩不抛）
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ztest.hpp"

#include "zenith/ai/audio_pcm.hpp"
#include "zenith/ai/whisper_stt.hpp"

using namespace zenith;

namespace {

void putU16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void putU32(std::string& s, uint32_t v) {
    s.push_back(static_cast<char>(v & 0xFF));
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>((v >> 16) & 0xFF));
    s.push_back(static_cast<char>((v >> 24) & 0xFF));
}

/// 生成 WAV：PCM16，指定采样率/声道，正弦波（每声道同幅值）
std::string makeWav(int sampleRate, int channels, double seconds, double freq, double amp) {
    const size_t frames = static_cast<size_t>(seconds * sampleRate);
    const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * 2);
    std::string wav;
    wav.reserve(44 + dataBytes);
    wav += "RIFF";
    putU32(wav, 36 + dataBytes);
    wav += "WAVE";
    wav += "fmt ";
    putU32(wav, 16);
    putU16(wav, 1);  // PCM
    putU16(wav, static_cast<uint16_t>(channels));
    putU32(wav, static_cast<uint32_t>(sampleRate));
    putU32(wav, static_cast<uint32_t>(sampleRate * channels * 2));  // byte rate
    putU16(wav, static_cast<uint16_t>(channels * 2));               // block align
    putU16(wav, 16);                                                // bits
    wav += "data";
    putU32(wav, dataBytes);
    for (size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / sampleRate;
        const auto v =
            static_cast<int16_t>(std::sin(2 * 3.14159265358979 * freq * t) * amp * 32767);
        for (int c = 0; c < channels; ++c)
            putU16(wav, static_cast<uint16_t>(v));
    }
    return wav;
}

}  // namespace

ZTEST(AudioPcm, ParseWavHeader) {
    const std::string wav = makeWav(44100, 2, 0.1, 440.0, 0.5);
    const auto info = ai::parseWavHeader(wav);
    ZEXPECT_TRUE(info.ok);
    ZEXPECT_EQ(info.formatTag, 1);
    ZEXPECT_EQ(info.channels, 2);
    ZEXPECT_EQ(info.sampleRate, 44100);
    ZEXPECT_EQ(info.bitsPerSample, 16);
    ZEXPECT_EQ(info.dataOffset, size_t{44});
    ZEXPECT_EQ(info.dataBytes, static_cast<size_t>(4410 * 2 * 2));
}

ZTEST(AudioPcm, DecodeMonoAmplitudeAndDuration) {
    const std::string wav = makeWav(16000, 1, 1.0, 440.0, 0.5);
    const auto pcm = ai::decodeAudioTo16kMono(wav, "wav");
    ZEXPECT_TRUE(pcm.ok);
    ZEXPECT_EQ(pcm.sourceRate, 16000);
    ZEXPECT_EQ(pcm.sourceChannels, 1);
    // 1 秒 @16k → 16000 个样本（允许 ±1 的取整误差）
    ZEXPECT_TRUE(pcm.samples.size() >= 15999 && pcm.samples.size() <= 16001);
    ZEXPECT_TRUE(pcm.durationMs >= 999 && pcm.durationMs <= 1001);
    // 幅度接近 0.5（正弦峰值）
    float peak = 0.0f;
    for (float s : pcm.samples)
        peak = std::max(peak, std::fabs(s));
    ZEXPECT_TRUE(peak > 0.45f && peak < 0.55f);
    // 采样值必须落在 [-1,1]
    for (float s : pcm.samples)
        ZEXPECT_TRUE(s >= -1.0f && s <= 1.0f);
}

ZTEST(AudioPcm, DecodeStereoDownmix) {
    const std::string wav = makeWav(16000, 2, 0.5, 440.0, 0.4);
    const auto pcm = ai::decodeAudioTo16kMono(wav, "wav");
    ZEXPECT_TRUE(pcm.ok);
    ZEXPECT_EQ(pcm.sourceChannels, 2);
    ZEXPECT_EQ(pcm.samples.size(), size_t{8000});  // 立体的帧数 = 单声道样本数
    float peak = 0.0f;
    for (float s : pcm.samples)
        peak = std::max(peak, std::fabs(s));
    // 两声道同相 → 平均后幅度不变（0.4）
    ZEXPECT_TRUE(peak > 0.35f && peak < 0.45f);
}

ZTEST(AudioPcm, Resample48kTo16k) {
    const std::string wav = makeWav(48000, 1, 0.25, 440.0, 0.5);
    const auto pcm = ai::decodeAudioTo16kMono(wav, "wav");
    ZEXPECT_TRUE(pcm.ok);
    ZEXPECT_EQ(pcm.sourceRate, 48000);
    // 0.25s → 4000 个 16k 样本
    ZEXPECT_TRUE(pcm.samples.size() >= 3999 && pcm.samples.size() <= 4001);
}

ZTEST(AudioPcm, RawPcm16Path) {
    std::string raw;
    for (int i = 0; i < 1600; ++i)
        putU16(raw, static_cast<uint16_t>(i * 10));
    const auto pcm = ai::decodeAudioTo16kMono(raw, "pcm16");
    ZEXPECT_TRUE(pcm.ok);
    ZEXPECT_EQ(pcm.samples.size(), size_t{1600});
    ZEXPECT_EQ(pcm.durationMs, 100);
}

ZTEST(AudioPcm, RejectsGarbage) {
    ZEXPECT_FALSE(ai::decodeAudioTo16kMono("", "wav").ok);
    ZEXPECT_FALSE(ai::decodeAudioTo16kMono("not a wav at all", "wav").ok);
    ZEXPECT_FALSE(ai::decodeAudioTo16kMono(std::string(100, 'x'), "mp3").ok);  // 不支持格式
    // 合法 RIFF 但无 data 块
    std::string broken = "RIFF";
    putU32(broken, 4);
    broken += "WAVE";
    ZEXPECT_FALSE(ai::parseWavHeader(broken).ok);
}

ZTEST(WhisperStt, GracefulWhenUnavailable) {
    // 默认构建不编入 whisper：工厂必须返回 nullptr + 可读原因，而不是崩溃
    ai::WhisperOptions opt;
    opt.modelPath = "does-not-exist.gguf";
    const auto stt = ai::makeWhisperStt(opt);
    ZEXPECT_TRUE(stt == nullptr);
    ZEXPECT_FALSE(ai::whisperLastError().empty());
    if (!ai::whisperCompiledIn()) {
        // 未编入时原因应指向构建开关（运维可据此自查）
        ZEXPECT_TRUE(ai::whisperLastError().find("ZENITH_WHISPER_ROOT") != std::string::npos);
    }
}

ZTEST_MAIN
