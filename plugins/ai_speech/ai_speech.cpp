// ============================================================================
//  ai_speech —— C ABI 插件：语音接口 + mock 实装（M13 第二批）
//  注册两个服务：
//    zenith.ai.stt → ISpeechToText（mock；whisper.cpp C ABI 后续同接口替换）
//    zenith.ai.tts → ITextToSpeech（mock 16kHz WAV；kokoro 后续同接口替换）
//  配置 ai.speech.backend = "mock"（默认；其他值当前回落 mock 并告警）。
// ============================================================================
#include <memory>

#include "zenith/abi/zenith_abi.h"

#include "zenith/ai/speech_service.hpp"

namespace {

const zenith_host_api* g_host = nullptr;
// shared_ptr 持有：注册用 no-op destroyer，生命周期=插件全局（shutdown 置空）
std::shared_ptr<zenith::ai::ISpeechToText> g_stt;
std::shared_ptr<zenith::ai::ITextToSpeech> g_tts;

void noopDestroy(void*) {}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    std::string backend;
    const char* v = nullptr;
    if (host->config_get(host->config_ctx, "ai.speech.backend", "", &v) == ZENITH_OK &&
        v != nullptr) {
        backend = v;
    }
    if (backend.empty()) backend = "mock";
    if (backend != "mock") {
        host->log_write(host->log_ctx, ZENITH_LOG_WARN, "ai_speech", 0,
                        ("ai_speech: backend '" + backend +
                         "' not bundled yet — falling back to mock")
                            .c_str());
    }

    g_stt = zenith::ai::makeMockStt();
    g_tts = zenith::ai::makeMockTts();
    if (g_stt == nullptr || g_tts == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;

    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_speech", 0,
                    ("ai_speech: ready (backend=" + backend + "; stt+tts registered)").c_str());
    const zenith_status r1 = host->register_service(
        host->registry_ctx, host->plugin_token,
        zenith::ai::ISpeechToText::service_name(),
        zenith::ai::ISpeechToText::service_version(),
        static_cast<void*>(g_stt.get()), &noopDestroy);
    if (r1 != ZENITH_OK) return r1;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::ai::ITextToSpeech::service_name(),
                                  zenith::ai::ITextToSpeech::service_version(),
                                  static_cast<void*>(g_tts.get()), &noopDestroy);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    g_stt.reset();
    g_tts.reset();
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_speech",
    "0.1.0",
    "Zenith speech services: ISpeechToText/ITextToSpeech with playable mock WAV backend",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
