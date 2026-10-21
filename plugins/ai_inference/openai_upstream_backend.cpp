// ============================================================================
//  OpenAiUpstreamBackend —— 把「任何 OpenAI 兼容服务」当成推理后端（M8.1）
//
//  为什么要这一层：本机用的是 **Prism ML fork 的 llama.cpp b10709**（支持
//  Ternary-Bonsai-2-27B 的 PQ2_0 三元量化 + `--kv-mean-center` KV 偏置），
//  该发行版只有预编译 DLL、没有 llama.h / 导入库，进程内链接会踩结构体布局
//  不匹配。但它自带 `llama-server.exe`，OpenAI 兼容面齐全——于是 Zenith 网关
//  以「上游」形态接它：网关负责鉴权/限流/路由/多模型/token 统计，上游负责算。
//  同一后端也顺带支持 vLLM / Ollama / 云端 OpenAI 兼容 API。
// ============================================================================
#include "backend.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

#include "upstream_http.hpp"
#include "zenith/json/json.hpp"

namespace zenith::ai::detail {
namespace {

using json::JsonValue;

std::string stripTrailingSlash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string trimLeft(std::string s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t')) ++b;
    return s.substr(b);
}

/// 上游没给 usage 时的粗估（≈4 字符/token）
int estimateTokens(const std::string& text) {
    return static_cast<int>(text.size() / 4) + 1;
}

int readInt(const JsonValue& node, int fallback) {
    return node.isNumber() ? static_cast<int>(node.asInt(fallback)) : fallback;
}

/// llama.cpp / NInfer 都在响应里带 `timings`（投机解码草稿数、预填/解码速率）
void readTimings(const JsonValue& node, EngineStats& out) {
    if (!node.isObject()) return;
    out.prefillTps = node["prompt_per_second"].asDouble(out.prefillTps);
    out.decodeTps = node["predicted_per_second"].asDouble(out.decodeTps);
    out.draftTotal = node["draft_n"].asInt(out.draftTotal);
    out.draftAccepted = node["draft_n_accepted"].asInt(out.draftAccepted);
    out.cachedTokens = node["cache_n"].asInt(out.cachedTokens);
    out.valid = true;
}

class OpenAiUpstreamBackend final : public Backend {
public:
    OpenAiUpstreamBackend(std::string baseUrl, std::string apiKey, std::string model,
                          int timeoutMs)
        : base_(stripTrailingSlash(std::move(baseUrl))),
          apiKey_(std::move(apiKey)),
          model_(std::move(model)),
          timeoutMs_(timeoutMs > 0 ? timeoutMs : 300000) {}

    [[nodiscard]] const char* name() const noexcept override { return "openai-upstream"; }

    [[nodiscard]] std::vector<ModelInfo> models() const override {
        UpstreamCall call;
        call.method = "GET";
        call.url = base_ + "/v1/models";
        call.headers = headers("application/json");
        call.timeoutMs = 15000;

        UpstreamResult res;
        std::vector<ModelInfo> out;
        if (httpRequest(call, nullptr, res) && res.status == 200) {
            auto parsed = JsonValue::parse(res.body);
            if (parsed) {
                const auto* data = parsed.value()["data"].asArray();
                if (data != nullptr) {
                    for (const auto& item : *data) {
                        ModelInfo info;
                        info.id = item["id"].asString();
                        if (info.id.empty()) continue;
                        info.ownedBy = "upstream";
                        info.createdAt = item["created"].asInt(0);
                        info.backend = "openai-upstream";
                        out.push_back(std::move(info));
                    }
                }
            }
        }
        if (out.empty() && !model_.empty()) { // 上游不可达/未实现 /v1/models 时兜底
            ModelInfo info;
            info.id = model_;
            info.ownedBy = "upstream";
            info.backend = "openai-upstream";
            out.push_back(std::move(info));
        }
        return out;
    }

    ChatResult chat(const ChatRequest& req, const TokenCallback& onToken,
                    const TokenCallback& onReasoning) override {
        ChatResult r;
        r.model = req.model.empty() ? model_ : req.model;

        JsonValue messages = JsonValue::array();
        int promptChars = 0;
        for (const auto& m : req.messages) {
            JsonValue item = JsonValue::object();
            item["role"] = m.role;
            item["content"] = m.content;
            if (!m.name.empty()) item["name"] = m.name;
            messages.push_back(std::move(item));
            promptChars += static_cast<int>(m.content.size());
        }

        JsonValue body = JsonValue::object();
        body["model"] = r.model;
        body["messages"] = std::move(messages);
        body["temperature"] = req.temperature;
        body["top_p"] = req.topP;
        body["max_tokens"] = req.maxTokens;
        body["stream"] = (onToken != nullptr) || (onReasoning != nullptr);
        if (req.seed >= 0) body["seed"] = req.seed;
        if (!req.stop.empty()) {
            JsonValue stops = JsonValue::array();
            for (const auto& s : req.stop) stops.push_back(s);
            body["stop"] = std::move(stops);
        }

        UpstreamCall call;
        call.method = "POST";
        call.url = base_ + "/v1/chat/completions";
        call.headers = headers("application/json");
        call.body = body.dump();
        call.timeoutMs = timeoutMs_;

        UpstreamResult res;
        std::string errorBody;
        std::string sseBuffer;
        bool aborted = false;
        bool sawDone = false;

        auto sink = [&](std::string_view chunk) -> bool {
            if (res.status != 200) {
                errorBody.append(chunk.data(), chunk.size());
                return true;
            }
            if (onToken == nullptr && onReasoning == nullptr) {
                errorBody.append(chunk.data(), chunk.size()); // 非流式：正文先攒这里
                return true;
            }
            return consumeSse(sseBuffer, chunk, onToken, onReasoning, r, aborted, sawDone);
        };

        const bool ok = httpRequest(call, sink, res);
        if (!ok) {
            r.error = res.error.empty() ? "upstream request failed" : res.error;
            return r;
        }
        if (res.status != 200) {
            r.error = "upstream HTTP " + std::to_string(res.status) + ": " + truncate(errorBody);
            return r;
        }

        if (onToken != nullptr || onReasoning != nullptr) {
            r.ok = !r.content.empty();
            if (!r.ok && !aborted) r.error = "upstream returned no content";
            if (aborted) r.finishReason = "aborted";
            if (r.usage.promptTokens == 0) r.usage.promptTokens = promptChars / 4 + 1;
            if (r.usage.completionTokens == 0) r.usage.completionTokens = estimateTokens(r.content);
            return r;
        }

        // 非流式：解析完整响应
        auto parsed = JsonValue::parse(errorBody);
        if (!parsed) {
            r.error = "upstream returned invalid JSON: " + truncate(errorBody);
            return r;
        }
        const JsonValue& root = parsed.value();
        if (root.has("error")) {
            r.error = "upstream error: " + truncate(root["error"].dump());
            return r;
        }
        const auto* choices = root["choices"].asArray();
        if (choices == nullptr || choices->empty()) {
            r.error = "upstream response has no choices";
            return r;
        }
        const JsonValue& first = (*choices)[0];
        if (const JsonValue* fr = first.find("finish_reason"); fr != nullptr && fr->isString()) {
            r.finishReason = fr->asString();
        }
        r.content = first["message"]["content"].asString();
        r.reasoning = first["message"]["reasoning_content"].asString(); // 思考模型
        if (const JsonValue* timings = root.find("timings"); timings != nullptr) {
            readTimings(*timings, r.stats);
        }
        if (const JsonValue* usage = root.find("usage"); usage != nullptr) {
            r.usage.promptTokens = readInt((*usage)["prompt_tokens"], 0);
            r.usage.completionTokens = readInt((*usage)["completion_tokens"], 0);
        }
        if (r.usage.promptTokens == 0) r.usage.promptTokens = promptChars / 4 + 1;
        if (r.usage.completionTokens == 0) r.usage.completionTokens = estimateTokens(r.content);
        r.ok = true;
        return r;
    }

    EmbeddingResult embeddings(const std::string& model, const std::string& input) override {
        EmbeddingResult r;
        r.model = model.empty() ? model_ : model;

        JsonValue body = JsonValue::object();
        body["model"] = r.model;
        body["input"] = input;

        UpstreamCall call;
        call.method = "POST";
        call.url = base_ + "/v1/embeddings";
        call.headers = headers("application/json");
        call.body = body.dump();
        call.timeoutMs = std::min(timeoutMs_, 60000);

        UpstreamResult res;
        if (!httpRequest(call, nullptr, res)) {
            r.error = res.error.empty() ? "upstream request failed" : res.error;
            return r;
        }
        if (res.status != 200) {
            r.error = "upstream HTTP " + std::to_string(res.status) + ": " + truncate(res.body);
            return r;
        }
        auto parsed = JsonValue::parse(res.body);
        if (!parsed) {
            r.error = "upstream returned invalid JSON";
            return r;
        }
        const auto* data = parsed.value()["data"].asArray();
        if (data == nullptr || data->empty()) {
            r.error = "upstream response has no embedding data";
            return r;
        }
        const auto* vec = (*data)[0]["embedding"].asArray();
        if (vec == nullptr) {
            r.error = "upstream embedding is not an array";
            return r;
        }
        r.vector.reserve(vec->size());
        for (const auto& v : *vec) r.vector.push_back(static_cast<float>(v.asDouble()));
        if (const JsonValue* usage = parsed.value().find("usage"); usage != nullptr) {
            r.usage.promptTokens = readInt((*usage)["prompt_tokens"], 0);
        }
        r.ok = !r.vector.empty();
        if (!r.ok) r.error = "upstream returned an empty embedding";
        return r;
    }

private:
    static std::string truncate(const std::string& s) {
        return s.size() <= 400 ? s : s.substr(0, 400) + "...";
    }

    [[nodiscard]] std::vector<std::pair<std::string, std::string>> headers(
        const char* accept) const {
        std::vector<std::pair<std::string, std::string>> h;
        h.emplace_back("Accept", accept);
        if (!apiKey_.empty()) h.emplace_back("Authorization", "Bearer " + apiKey_);
        return h;
    }

    /// 增量解析 SSE：data: {...} → delta.content 逐片回调
    static bool consumeSse(std::string& pending, std::string_view chunk, const TokenCallback& onToken,
                           const TokenCallback& onReasoning, ChatResult& r, bool& aborted,
                           bool& sawDone) {
        pending.append(chunk.data(), chunk.size());
        for (;;) {
            const size_t eol = pending.find('\n');
            if (eol == std::string::npos) return true;
            std::string line = pending.substr(0, eol);
            pending.erase(0, eol + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("data:", 0) != 0) continue; // event:/id:/空行/心跳注释
            std::string payload = trimLeft(line.substr(5));
            if (payload.empty()) continue;
            if (payload == "[DONE]") {
                sawDone = true;
                return false; // 收尾：让 httpRequest 关连接
            }
            auto parsed = json::JsonValue::parse(payload);
            if (!parsed) continue;
            const json::JsonValue& v = parsed.value();
            if (const json::JsonValue* timings = v.find("timings"); timings != nullptr) {
                readTimings(*timings, r.stats); // 流式：末片通常带 timings
            }
            if (const json::JsonValue* usage = v.find("usage"); usage != nullptr &&
                                                              usage->isObject()) {
                r.usage.promptTokens = readInt((*usage)["prompt_tokens"], r.usage.promptTokens);
                r.usage.completionTokens =
                    readInt((*usage)["completion_tokens"], r.usage.completionTokens);
            }
            const auto* choices = v["choices"].asArray();
            if (choices == nullptr || choices->empty()) continue;
            const json::JsonValue& c0 = (*choices)[0];
            if (const json::JsonValue* fr = c0.find("finish_reason");
                fr != nullptr && fr->isString()) {
                r.finishReason = fr->asString();
            }

            // 思考模型的推理过程（llama.cpp 扩展字段）
            const json::JsonValue& thinking = c0["delta"]["reasoning_content"];
            if (thinking.isString() && !thinking.asString().empty()) {
                const std::string piece = thinking.asString();
                r.reasoning += piece;
                if (onReasoning && !onReasoning(piece)) {
                    aborted = true;
                    return false;
                }
            }

            const json::JsonValue& delta = c0["delta"]["content"];
            if (!delta.isString()) continue;
            const std::string piece = delta.asString();
            if (piece.empty()) continue;
            r.content += piece;
            if (onToken && !onToken(piece)) {
                aborted = true;
                return false;
            }
        }
    }

    std::string base_;
    std::string apiKey_;
    std::string model_;
    int timeoutMs_;
};

} // namespace

std::unique_ptr<Backend> makeOpenAiUpstreamBackend(std::string baseUrl, std::string apiKey,
                                                   std::string model, int timeoutMs) {
    if (baseUrl.empty()) return nullptr;
    return std::make_unique<OpenAiUpstreamBackend>(std::move(baseUrl), std::move(apiKey),
                                                   std::move(model), timeoutMs);
}

} // namespace zenith::ai::detail
