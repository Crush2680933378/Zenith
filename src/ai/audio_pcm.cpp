// ============================================================================
//  语音前处理实现（M8.8）：WAV 解析 / 拆声道降混 / 线性重采样
// ============================================================================
#include "zenith/ai/audio_pcm.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace zenith::ai {
namespace {

constexpr int kTargetRate = 16000;

uint16_t rdU16(const char* p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(p[0]) |
                                 (static_cast<unsigned char>(p[1]) << 8));
}
uint32_t rdU32(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(p[3])) << 24);
}

/// 单样本归一化到 [-1,1]
float sampleAt(const char* p, int bits, int tag) {
    if (tag == 3) { // IEEE float
        if (bits == 32) {
            float v = 0.0f;
            std::memcpy(&v, p, sizeof(v));
            return std::clamp(v, -1.0f, 1.0f);
        }
        if (bits == 64) {
            double v = 0.0;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(std::clamp(v, -1.0, 1.0));
        }
        return 0.0f;
    }
    switch (bits) {
        case 8:
            // 8bit WAV 是无符号的偏置编码
            return (static_cast<int>(static_cast<unsigned char>(p[0])) - 128) / 128.0f;
        case 16: {
            const auto v = static_cast<int16_t>(rdU16(p));
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            int32_t v = static_cast<int32_t>(static_cast<unsigned char>(p[0])) |
                        (static_cast<int32_t>(static_cast<unsigned char>(p[1])) << 8) |
                        (static_cast<int32_t>(static_cast<unsigned char>(p[2])) << 16);
            if ((v & 0x800000) != 0) v |= static_cast<int32_t>(0xFF000000); // 符号扩展
            return static_cast<float>(v) / 8388608.0f;
        }
        case 32: {
            const auto v = static_cast<int32_t>(rdU32(p));
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
        default:
            return 0.0f;
    }
}

std::vector<float> resampleLinear(const std::vector<float>& in, int srcRate, int dstRate) {
    if (srcRate <= 0 || dstRate <= 0 || in.empty()) return {};
    if (srcRate == dstRate) return in;
    const size_t outN = static_cast<size_t>(
        std::llround(static_cast<double>(in.size()) * dstRate / srcRate));
    std::vector<float> out(outN);
    const double step = static_cast<double>(srcRate) / dstRate;
    for (size_t i = 0; i < outN; ++i) {
        const double pos = static_cast<double>(i) * step;
        const size_t i0 = static_cast<size_t>(pos);
        if (i0 + 1 >= in.size()) {
            out[i] = in.empty() ? 0.0f : in.back();
            continue;
        }
        const double frac = pos - static_cast<double>(i0);
        out[i] = static_cast<float>(in[i0] * (1.0 - frac) + in[i0 + 1] * frac);
    }
    return out;
}

PcmAudio fail(PcmAudio r, std::string err) {
    r.ok = false;
    r.error = std::move(err);
    return r;
}

} // namespace

WavInfo parseWavHeader(std::string_view bytes) {
    WavInfo w;
    if (bytes.size() < 12) {
        w.error = "too short for RIFF header";
        return w;
    }
    if (std::memcmp(bytes.data(), "RIFF", 4) != 0 || std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        w.error = "not a RIFF/WAVE stream";
        return w;
    }
    size_t pos = 12;
    bool haveFmt = false;
    bool haveData = false;
    while (pos + 8 <= bytes.size() && !(haveFmt && haveData)) {
        const char* id = bytes.data() + pos;
        const uint32_t size = rdU32(bytes.data() + pos + 4);
        const size_t body = pos + 8;
        if (std::memcmp(id, "fmt ", 4) == 0) {
            if (size < 16 || body + 16 > bytes.size()) {
                w.error = "truncated fmt chunk";
                return w;
            }
            w.formatTag = rdU16(bytes.data() + body);
            w.channels = rdU16(bytes.data() + body + 2);
            w.sampleRate = static_cast<int>(rdU32(bytes.data() + body + 4));
            w.bitsPerSample = rdU16(bytes.data() + body + 14);
            // extensible：真正格式在 SubFormat GUID 前两字节
            if (w.formatTag == 0xFFFE && size >= 40 && body + 26 <= bytes.size()) {
                w.formatTag = rdU16(bytes.data() + body + 24);
            }
            haveFmt = true;
        } else if (std::memcmp(id, "data", 4) == 0) {
            w.dataOffset = body;
            w.dataBytes = std::min<size_t>(size, bytes.size() - body);
            haveData = true;
        }
        // chunk 按偶数字节对齐
        pos = body + size + (size & 1u);
    }
    if (!haveFmt) {
        w.error = "missing fmt chunk";
        return w;
    }
    if (!haveData) {
        w.error = "missing data chunk";
        return w;
    }
    if (w.channels <= 0 || w.sampleRate <= 0) {
        w.error = "invalid channel count / sample rate";
        return w;
    }
    w.ok = true;
    return w;
}

PcmAudio decodeAudioTo16kMono(std::string_view bytes, const std::string& format) {
    PcmAudio out;
    if (bytes.empty()) return fail(std::move(out), "empty audio");

    std::string fmt = format;
    for (char& c : fmt) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (fmt.empty()) {
        fmt = (bytes.size() >= 12 && std::memcmp(bytes.data(), "RIFF", 4) == 0) ? "wav" : "pcm16";
    }

    if (fmt == "pcm16") {
        const size_t frames = bytes.size() / 2;
        out.samples.reserve(frames);
        for (size_t i = 0; i < frames; ++i) {
            out.samples.push_back(sampleAt(bytes.data() + i * 2, 16, 1));
        }
        out.sourceRate = kTargetRate;
        out.sourceChannels = 1;
        out.ok = true;
        out.durationMs = static_cast<int>(frames * 1000 / kTargetRate);
        return out;
    }
    if (fmt != "wav") return fail(std::move(out), "unsupported audio format: " + format);

    const WavInfo w = parseWavHeader(bytes);
    if (!w.ok) return fail(std::move(out), "wav: " + w.error);
    if (w.formatTag != 1 && w.formatTag != 3) {
        return fail(std::move(out),
                    "wav: unsupported format tag " + std::to_string(w.formatTag) + " (need PCM/IEEE float)");
    }
    const int bytesPer = w.bitsPerSample / 8;
    if (bytesPer <= 0) return fail(std::move(out), "wav: invalid bits per sample");
    const size_t frameBytes = static_cast<size_t>(bytesPer) * static_cast<size_t>(w.channels);
    if (frameBytes == 0) return fail(std::move(out), "wav: invalid block align");
    const size_t frames = w.dataBytes / frameBytes;
    if (frames == 0) return fail(std::move(out), "wav: no sample frames");

    // 降混：各声道取平均（对语音足够，不做相位处理）
    std::vector<float> mono(frames);
    const char* base = bytes.data() + w.dataOffset;
    for (size_t i = 0; i < frames; ++i) {
        const char* frame = base + i * frameBytes;
        float sum = 0.0f;
        for (int c = 0; c < w.channels; ++c) {
            sum += sampleAt(frame + static_cast<size_t>(c) * bytesPer, w.bitsPerSample, w.formatTag);
        }
        mono[i] = sum / static_cast<float>(w.channels);
    }

    out.samples = resampleLinear(mono, w.sampleRate, kTargetRate);
    out.sourceRate = w.sampleRate;
    out.sourceChannels = w.channels;
    out.ok = true;
    out.durationMs = static_cast<int>(out.samples.size() * 1000 / kTargetRate);
    return out;
}

} // namespace zenith::ai
