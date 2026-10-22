// ============================================================================
//  MockBackend —— 确定性推理后端（M8.1）
//  作用：没有模型/没有 llama.cpp 时，OpenAI 兼容层的契约（JSON 形状、SSE 帧、
//  usage、中止语义）依然可被全链路验证 —— CI 里跑的就是这套。
// ============================================================================
#include <algorithm>
#include <cmath>
#include <cstdint>

#include "backend.hpp"

namespace zenith::ai::detail {

namespace {

/// 粗略切词（空格/制表/换行分隔），仅用于 usage 计数
int countWords(const std::string& s) {
    int n = 0;
    bool inWord = false;
    for (char ch : s) {
        const bool sep = ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
        if (!sep && !inWord) ++n;
        inWord = !sep;
    }
    return n;
}

/// 固定宽度切片：保证流式分片拼接后与完整回复逐字节一致。
/// 必须在 UTF-8 字符边界收口——按字节硬切会把中文/emoji 劈成非法序列，
/// 客户端 JSON 解析后就是乱码（GUI 上表现为 "???"）。
std::vector<std::string> sliceTokens(const std::string& text, size_t width) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        size_t end = std::min(text.size(), i + width);
        // 0b10xxxxxx 是续字节：往后推到下一个字符起始，避免切断多字节序列
        while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) ++end;
        out.push_back(text.substr(i, end - i));
        i = end;
    }
    return out;
}

class MockBackend final : public Backend {
public:
    MockBackend(std::string modelId, int embeddingDim)
        : modelId_(std::move(modelId)), embeddingDim_(embeddingDim > 0 ? embeddingDim : 64) {}

    [[nodiscard]] const char* name() const noexcept override { return "mock"; }

    [[nodiscard]] std::vector<ModelInfo> models() const override {
        ModelInfo m;
        m.id = modelId_;
        m.ownedBy = "zenith";
        m.createdAt = 1790000000; // 固定时间，便于断言
        m.contextSize = 4096;
        m.backend = "mock";
        return {m};
    }

    ChatResult chat(const ChatRequest& req, const TokenCallback& onToken,
                    const TokenCallback& onReasoning) override {
        (void)onReasoning; // mock 不产生推理过程
        ChatResult r;
        r.model = req.model.empty() ? modelId_ : req.model;

        std::string lastUser;
        for (const auto& m : req.messages) {
            r.usage.promptTokens += countWords(m.content) + 1; // +1 ≈ 角色标记
            if (m.role == "user") lastUser = m.content;
        }

        const std::string reply = "mock: " + lastUser;
        const std::vector<std::string> pieces = sliceTokens(reply, 4);
        const int limit = req.maxTokens > 0 ? req.maxTokens : 512;

        for (int i = 0; i < static_cast<int>(pieces.size()); ++i) {
            if (i >= limit) {
                r.finishReason = "length";
                break;
            }
            r.content += pieces[static_cast<size_t>(i)];
            r.usage.completionTokens += 1;
            if (onToken && !onToken(pieces[static_cast<size_t>(i)])) {
                r.finishReason = "aborted";
                break;
            }
            bool hitStop = false;
            for (const auto& s : req.stop) {
                if (!s.empty() && r.content.find(s) != std::string::npos) {
                    hitStop = true;
                    break;
                }
            }
            if (hitStop) {
                r.finishReason = "stop";
                break;
            }
        }

        r.ok = true;
        r.id = "chatcmpl-mock";
        return r;
    }

    EmbeddingResult embeddings(const std::string& model, const std::string& input) override {
        EmbeddingResult r;
        r.model = model.empty() ? modelId_ : model;
        r.vector.resize(static_cast<size_t>(embeddingDim_));
        r.usage.promptTokens = countWords(input) + 1;

        // FNV-1a 逐维散列 → [-1, 1]，再单位化；同一输入恒定同一向量
        double norm = 0.0;
        for (int i = 0; i < embeddingDim_; ++i) {
            uint64_t h = 1469598103934665603ull ^ (static_cast<uint64_t>(i) * 0x9E3779B97F4A7C15ull);
            for (unsigned char ch : input) {
                h ^= ch;
                h *= 1099511628211ull;
            }
            const double v = static_cast<double>(h % 2001) / 1000.0 - 1.0;
            r.vector[static_cast<size_t>(i)] = static_cast<float>(v);
            norm += v * v;
        }
        norm = std::sqrt(norm);
        if (norm > 0.0) {
            for (auto& f : r.vector) f = static_cast<float>(f / norm);
        }
        r.ok = true;
        return r;
    }

private:
    std::string modelId_;
    int embeddingDim_;
};

} // namespace

std::unique_ptr<Backend> makeMockBackend(std::string modelId, int embeddingDim) {
    if (modelId.empty()) modelId = "zenith-mock-1";
    return std::make_unique<MockBackend>(std::move(modelId), embeddingDim);
}

} // namespace zenith::ai::detail
