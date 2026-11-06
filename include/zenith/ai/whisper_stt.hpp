// ============================================================================
//  zenith::ai::WhisperStt —— whisper.cpp 真 STT 后端（M8.8）
//  与 ISpeechToText 同契约：ai_speech 插件按配置装配，缺库/缺模型时回落 mock。
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "zenith/ai/speech_service.hpp"
#include "zenith/core/export.hpp"

namespace zenith::ai {

struct WhisperOptions {
    std::string modelPath;         // ggml 模型路径（必填）
    std::string language = "auto"; // "auto" 自动识别；否则 "zh"/"en"…
    int threads = 4;
    bool translate = false;        // true：转写并翻译成英文
};

/// 本次构建是否编入了 whisper.cpp（-DZENITH_WHISPER_ROOT=…）
ZENITH_API bool whisperCompiledIn();

/// 构造 whisper 后端。未编入 whisper 或模型加载失败时返回 nullptr，
/// 失败原因见 whisperLastError()（线程内）。
ZENITH_API std::shared_ptr<ISpeechToText> makeWhisperStt(const WhisperOptions& opt);

/// 最近一次构造失败的原因（便于插件日志/管理台展示）
ZENITH_API std::string whisperLastError();

} // namespace zenith::ai
