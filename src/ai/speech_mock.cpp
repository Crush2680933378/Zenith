// ============================================================================
//  mock 语音后端（M13 第二批）——零依赖联调实现
//  STT：回显字节数的固定转写；TTS：可播放的 16bit/16kHz 单声道 WAV
// ============================================================================
#include "zenith/ai/speech_service.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace zenith::ai {
namespace {

void putU16(std::string& s, uint16_t v) {
    const char b[2] = {static_cast<char>(v & 0xFF), static_cast<char>(v >> 8)};
    s.append(b, 2);
}
void putU32(std::string& s, uint32_t v) {
    const char b[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                       static_cast<char>((v >> 16) & 0xFF),
                       static_cast<char>((v >> 24) & 0xFF)};
    s.append(b, 4);
}

class MockStt final : public ISpeechToText {
public:
    std::string backend() const override { return "mock"; }
    SttResult transcribe(std::string_view audio, const std::string& format) override {
        SttResult r;
        if (audio.empty()) {
            r.error = "empty audio";
            return r;
        }
        r.ok = true;
        r.language = "zh";
        r.durationMs = static_cast<int>(audio.size() / 32); // 32KB/s 的粗估
        r.text = "[mock-stt " + format + "/" + std::to_string(audio.size()) +
                 "B] 你好，Zenith 语音链路就绪。";
        return r;
    }
};

class MockTts final : public ITextToSpeech {
public:
    std::string backend() const override { return "mock"; }
    TtsResult synthesize(const std::string& text, const std::string& voice,
                         const std::string& format) override {
        TtsResult r;
        if (text.empty()) {
            r.error = "empty text";
            return r;
        }
        if (format != "wav" && !format.empty()) {
            r.error = "mock tts only emits wav";
            return r;
        }
        // 时长 = 0.3s 起步 + 每字符 80ms（与文本长度单调相关，便于测试断言）
        const int sampleRate = 16000;
        const double seconds = 0.3 + text.size() * 0.08;
        const size_t samples = static_cast<size_t>(seconds * sampleRate);
        const uint32_t dataBytes = static_cast<uint32_t>(samples * 2);

        std::string wav;
        wav.reserve(44 + dataBytes);
        wav += "RIFF";
        putU32(wav, 36 + dataBytes);
        wav += "WAVE";
        wav += "fmt ";
        putU32(wav, 16);          // PCM chunk size
        putU16(wav, 1);           // PCM
        putU16(wav, 1);           // mono
        putU32(wav, sampleRate);
        putU32(wav, sampleRate * 2); // byte rate
        putU16(wav, 2);           // block align
        putU16(wav, 16);          // bits
        wav += "data";
        putU32(wav, dataBytes);

        // 声纹区分：voice 哈希决定基频（zf_/zm_ 前缀之外也能出声）
        uint32_t h = 440;
        for (char c : voice) h = h * 131 + static_cast<unsigned char>(c);
        const double freq = 300.0 + (h % 400);
        for (size_t i = 0; i < samples; ++i) {
            const double t = static_cast<double>(i) / sampleRate;
            const double env = // 20ms 淡入淡出
                std::min(1.0, t / 0.02) * std::min(1.0, (seconds - t) / 0.02);
            const double wave =
                0.5 * std::sin(2 * 3.14159265358979 * freq * t) +
                0.2 * std::sin(2 * 3.14159265358979 * freq * 2 * t);
            const int16_t s = static_cast<int16_t>(wave * env * 12000);
            putU16(wav, static_cast<uint16_t>(s));
        }
        r.ok = true;
        r.format = "wav";
        r.audio = std::move(wav);
        return r;
    }
};

} // namespace

std::shared_ptr<ISpeechToText> makeMockStt() {
    return std::make_shared<MockStt>();
}
std::shared_ptr<ITextToSpeech> makeMockTts() {
    return std::make_shared<MockTts>();
}

} // namespace zenith::ai
