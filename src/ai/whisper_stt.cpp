// ============================================================================
//  zenith::ai::WhisperStt —— whisper.cpp 真 STT 后端（M8.8）
//
//  设计取舍：
//   - **编译期可选**：构建时给 -DZENITH_WHISPER_ROOT=<whisper.cpp 根目录> 才编入。
//     没有 whisper 时工厂返回 nullptr 并给出明确原因，插件据此回落 mock，
//     不会让「没装 whisper」变成编译失败或运行期崩溃。
//   - **模型按需加载**：一次加载常驻（whisper 初始化要几百 ms 到数秒），
//     转写串行化（whisper_context 非线程安全）。
//   - 输入统一经 audio_pcm 转成 16kHz 单声道 float——whisper 只认这个形态。
// ============================================================================
#include "zenith/ai/whisper_stt.hpp"

#include <mutex>
#include <utility>

#include "zenith/ai/audio_pcm.hpp"

#if defined(ZENITH_HAS_WHISPER)
#include "whisper.h"
#endif

namespace zenith::ai {
namespace {

thread_local std::string gLastError;

void setError(std::string e) { gLastError = std::move(e); }

std::string trimAscii(std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\n' || s[b] == '\r' || s[b] == '\t')) ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

#if defined(ZENITH_HAS_WHISPER)
class WhisperStt final : public ISpeechToText {
public:
    WhisperStt(whisper_context* ctx, WhisperOptions opt) : ctx_(ctx), opt_(std::move(opt)) {}
    ~WhisperStt() override {
        if (ctx_ != nullptr) whisper_free(ctx_);
    }
    WhisperStt(const WhisperStt&) = delete;
    WhisperStt& operator=(const WhisperStt&) = delete;

    std::string backend() const override { return "whisper"; }

    SttResult transcribe(std::string_view audio, const std::string& format) override {
        SttResult r;
        PcmAudio pcm = decodeAudioTo16kMono(audio, format);
        if (!pcm.ok) {
            r.error = pcm.error;
            return r;
        }
        if (pcm.samples.empty()) {
            r.error = "no samples after decode";
            return r;
        }
        if (pcm.samples.size() < 1600) { // < 100ms：whisper 会直接报错，提前给出人话
            r.error = "audio too short (<100ms)";
            return r;
        }

        // whisper_full 同时只允许一个线程操作同一 context
        std::lock_guard<std::mutex> lk(mtx_);
        whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        p.print_progress = false;
        p.print_realtime = false;
        p.print_timestamps = false;
        p.print_special = false;
        p.translate = opt_.translate;
        p.n_threads = opt_.threads > 0 ? opt_.threads : 4;
        const std::string lang = opt_.language.empty() ? std::string("auto") : opt_.language;
        p.language = (lang == "auto") ? nullptr : lang.c_str();

        const int rc = whisper_full(ctx_, p, pcm.samples.data(),
                                    static_cast<int>(pcm.samples.size()));
        if (rc != 0) {
            r.error = "whisper_full failed (rc=" + std::to_string(rc) + ")";
            return r;
        }

        std::string text;
        const int segs = whisper_full_n_segments(ctx_);
        for (int i = 0; i < segs; ++i) {
            const char* t = whisper_full_get_segment_text(ctx_, i);
            if (t != nullptr) text += t;
        }
        r.ok = true;
        r.text = trimAscii(std::move(text));
        r.durationMs = pcm.durationMs;
        const int langId = whisper_full_lang_id(ctx_);
        if (langId >= 0) {
            const char* code = whisper_lang_str(langId);
            if (code != nullptr) r.language = code;
        }
        return r;
    }

private:
    whisper_context* ctx_;
    WhisperOptions opt_;
    std::mutex mtx_;
};
#endif // ZENITH_HAS_WHISPER

} // namespace

bool whisperCompiledIn() {
#if defined(ZENITH_HAS_WHISPER)
    return true;
#else
    return false;
#endif
}

std::string whisperLastError() { return gLastError; }

std::shared_ptr<ISpeechToText> makeWhisperStt(const WhisperOptions& opt) {
#if defined(ZENITH_HAS_WHISPER)
    if (opt.modelPath.empty()) {
        setError("whisper: model path is empty (config ai.speech.whisper.model)");
        return nullptr;
    }
    whisper_context_params cp = whisper_context_default_params();
    cp.use_gpu = false; // 服务端优先确定性；有 GPU 需求可后续开配置
    whisper_context* ctx = whisper_init_from_file_with_params(opt.modelPath.c_str(), cp);
    if (ctx == nullptr) {
        setError("whisper: failed to load model: " + opt.modelPath);
        return nullptr;
    }
    return std::make_shared<WhisperStt>(ctx, opt);
#else
    (void)opt;
    setError(
        "whisper: not compiled in — configure with -DZENITH_WHISPER_ROOT=<whisper.cpp root> "
        "(see docs/SPEECH.md)");
    return nullptr;
#endif
}

} // namespace zenith::ai
