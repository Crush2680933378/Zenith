// ============================================================================
//  zenith::ai 语音接口 —— ISpeechToText / ITextToSpeech（M13 第二批）
//
//  后端无关抽象：ai_stt(whisper.cpp C ABI) / ai_tts(kokoro) 插件实现同
//  接口注册服务；上层（网关 / crew / agent）只认抽象。
//    ISpeechToText::transcribe(audioBytes, "wav"|"pcm16"|"mp3")
//      → {text, language, durationMs}          （whisper 形态）
//    ITextToSpeech::synthesize(text, voice, "wav")
//      → {audioBytes}                          （kokoro 形态）
//  自带 mock 实现（零依赖、可听的正弦波 WAV），供联调/测试/演示。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"

namespace zenith::ai {

struct SttResult {
    bool ok = false;
    std::string text;       // 转写文本
    std::string language;   // "zh" / "en" / ""（未识别）
    int durationMs = 0;     // 音频时长（后端估计）
    std::string error;
};

struct TtsResult {
    bool ok = false;
    std::string audio;      // 音频字节（format 指定容器，默认 16bit/16kHz WAV）
    std::string format;     // "wav"
    std::string error;
};

class ISpeechToText {
public:
    virtual ~ISpeechToText() = default;
    /// 服务注册契约（ai_speech 插件 → 宿主/其他插件；同工具链 C++ 契约）
    static const char* service_name() { return "zenith.ai.stt"; }
    static uint32_t service_version() { return 1; }
    /// 后端标识（"whisper" / "mock"…）
    [[nodiscard]] virtual std::string backend() const = 0;
    virtual SttResult transcribe(std::string_view audio, const std::string& format) = 0;
};

class ITextToSpeech {
public:
    virtual ~ITextToSpeech() = default;
    static const char* service_name() { return "zenith.ai.tts"; }
    static uint32_t service_version() { return 1; }
    [[nodiscard]] virtual std::string backend() const = 0;
    virtual TtsResult synthesize(const std::string& text, const std::string& voice,
                                 const std::string& format) = 0;
};

/// mock：固定转写文本（含字节数回显，便于断言链路）
ZENITH_API std::shared_ptr<ISpeechToText> makeMockStt();
/// mock：真实可播 WAV（440Hz 正弦 + 包络；时长随文本长度增长）
ZENITH_API std::shared_ptr<ITextToSpeech> makeMockTts();

} // namespace zenith::ai
