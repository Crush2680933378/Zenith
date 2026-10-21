#include "zenith/ai/openai_api.hpp"

#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/http/context.hpp"
#include "zenith/http/sse.hpp"
#include "zenith/json/json.hpp"
#include "zenith/metrics/metrics.hpp"

namespace zenith::ai {

namespace {

using json::JsonValue;

// ---------------------------------------------------------------------------
//  小工具
// ---------------------------------------------------------------------------
int64_t nowSec() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string randomHex(size_t bytes) {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (size_t i = 0; i < bytes; ++i) {
        const uint64_t v = rng() & 0xFF;
        out.push_back(kHex[(v >> 4) & 0xF]);
        out.push_back(kHex[v & 0xF]);
    }
    return out;
}

void sendError(http::Context& c, int status, const char* type, const std::string& message) {
    JsonValue err = JsonValue::object();
    err["message"] = message;
    err["type"] = type;
    err["param"] = nullptr;
    err["code"] = nullptr;
    JsonValue root = JsonValue::object();
    root["error"] = err;
    c.json(status, root);
}

JsonValue usageObject(const Usage& u) {
    JsonValue v = JsonValue::object();
    v["prompt_tokens"] = u.promptTokens;
    v["completion_tokens"] = u.completionTokens;
    v["total_tokens"] = u.totalTokens();
    return v;
}

/// 请求体 → JSON（失败时已回 400）
bool parseBody(http::Context& c, JsonValue& out) {
    auto parsed = JsonValue::parse(c.body());
    if (!parsed) {
        sendError(c, 400, "invalid_request_error", "invalid JSON body: " + parsed.error().message);
        return false;
    }
    out = std::move(parsed).value();
    if (!out.isObject()) {
        sendError(c, 400, "invalid_request_error", "request body must be a JSON object");
        return false;
    }
    return true;
}

/// 消息内容兼容两种形态：字符串，或 [{type:"text",text:"..."}] 数组
bool readContent(const JsonValue& node, std::string& out, std::string& err) {
    if (node.isString()) {
        out = node.asString();
        return true;
    }
    if (node.isNull()) {
        out.clear(); // 允许 assistant 消息仅含 tool_calls 时内容为空
        return true;
    }
    const auto* parts = node.asArray();
    if (parts == nullptr) {
        err = "message.content must be a string or an array of parts";
        return false;
    }
    for (const auto& part : *parts) {
        if (part.isString()) {
            out += part.asString();
            continue;
        }
        const JsonValue* text = part.find("text");
        if (text != nullptr && text->isString()) out += text->asString();
    }
    return true;
}

bool buildChatRequest(const JsonValue& body, ChatRequest& req, bool& stream, std::string& err) {
    const JsonValue& messages = body["messages"];
    const auto* arr = messages.asArray();
    if (arr == nullptr) {
        err = "'messages' must be an array";
        return false;
    }
    if (arr->empty()) {
        err = "'messages' must not be empty";
        return false;
    }
    if (arr->size() > 512) {
        err = "'messages' exceeds the 512-message limit";
        return false;
    }
    for (const auto& m : *arr) {
        if (!m.isObject()) {
            err = "each message must be a JSON object";
            return false;
        }
        ChatMessage msg;
        msg.role = m["role"].asString();
        if (msg.role.empty()) {
            err = "message.role is required";
            return false;
        }
        if (!readContent(m["content"], msg.content, err)) return false;
        if (const JsonValue* name = m.find("name"); name != nullptr && name->isString()) {
            msg.name = name->asString();
        }
        req.messages.push_back(std::move(msg));
    }

    if (body.has("model")) req.model = body["model"].asString();
    if (body.has("temperature")) req.temperature = body["temperature"].asDouble(0.7);
    if (body.has("top_p")) req.topP = body["top_p"].asDouble(1.0);
    if (body.has("max_tokens")) req.maxTokens = static_cast<int>(body["max_tokens"].asInt(512));
    if (body.has("seed")) req.seed = static_cast<int>(body["seed"].asInt(-1));
    if (const JsonValue* stop = body.find("stop"); stop != nullptr) {
        if (stop->isString()) {
            req.stop.push_back(stop->asString());
        } else if (const auto* list = stop->asArray(); list != nullptr) {
            for (const auto& s : *list) {
                if (s.isString()) req.stop.push_back(s.asString());
            }
        }
    }
    stream = body["stream"].asBool(false);

    // 参数域收敛，避免落进后端做无意义计算
    if (req.temperature < 0.0) req.temperature = 0.0;
    if (req.temperature > 2.0) req.temperature = 2.0;
    if (req.topP <= 0.0) req.topP = 1.0;
    if (req.topP > 1.0) req.topP = 1.0;
    if (req.maxTokens <= 0) req.maxTokens = 512;
    if (req.maxTokens > 32768) req.maxTokens = 32768;
    return true;
}

void countRequest(bool enabled, const char* endpoint) {
    if (!enabled) return;
    auto& reg = metrics::Registry::instance();
    reg.counter("zenith_ai_requests_total").inc();
    reg.counter(std::string("zenith_ai_") + endpoint + "_requests_total").inc();
}

void countTokens(bool enabled, const Usage& usage) {
    if (!enabled) return;
    auto& reg = metrics::Registry::instance();
    reg.counter("zenith_ai_prompt_tokens_total").inc(usage.promptTokens);
    reg.counter("zenith_ai_completion_tokens_total").inc(usage.completionTokens);
}

/// 引擎侧测速读数（llama.cpp / NInfer 的 timings）→ Prometheus
void recordEngineStats(bool enabled, const EngineStats& stats) {
    if (!enabled || !stats.valid) return;
    auto& reg = metrics::Registry::instance();
    reg.gauge("zenith_ai_prefill_tps").set(static_cast<int64_t>(stats.prefillTps));
    reg.gauge("zenith_ai_decode_tps").set(static_cast<int64_t>(stats.decodeTps));
    reg.counter("zenith_ai_draft_tokens_total").inc(stats.draftTotal);
    reg.counter("zenith_ai_draft_accepted_total").inc(stats.draftAccepted);
    reg.counter("zenith_ai_cached_prompt_tokens_total").inc(stats.cachedTokens);
}

/// 引擎进程运行态 → Prometheus（每次访问 status/每次请求后刷新）
void recordEngineState(bool enabled, const EngineState& st) {
    if (!enabled) return;
    auto& reg = metrics::Registry::instance();
    reg.gauge("zenith_ai_engine_supervised").set(st.supervised ? 1 : 0);
    reg.gauge("zenith_ai_engine_running").set(st.running ? 1 : 0);
    reg.gauge("zenith_ai_engine_requests").set(st.requests);
    if (st.lastTtftMs > 0) reg.gauge("zenith_ai_ttft_ms").set(static_cast<int64_t>(st.lastTtftMs));
    if (st.lastPrefillTps > 0) {
        reg.gauge("zenith_ai_prefill_tps").set(static_cast<int64_t>(st.lastPrefillTps));
    }
    if (st.lastDecodeTps > 0) {
        reg.gauge("zenith_ai_decode_tps").set(static_cast<int64_t>(st.lastDecodeTps));
    }
}

/// 有线程池则投递，否则就地执行（池已关闭时回退就地，避免丢掉响应）
void runAsync(const OpenAiApiOptions& opt, std::function<void()> work) {
    if (opt.pool != nullptr && opt.pool->post(work)) return;
    work();
}

// ---------------------------------------------------------------------------
//  /v1/models
// ---------------------------------------------------------------------------
void handleModels(http::Context& c, IInferenceService& svc, const OpenAiApiOptions& opt) {
    countRequest(opt.metricsEnabled, "models");
    const int64_t created = nowSec();
    JsonValue data = JsonValue::array();
    for (const auto& m : svc.models()) {
        JsonValue item = JsonValue::object();
        item["id"] = m.id;
        item["object"] = "model";
        item["created"] = m.createdAt > 0 ? m.createdAt : created;
        item["owned_by"] = m.ownedBy;
        JsonValue meta = JsonValue::object();
        meta["backend"] = m.backend;
        meta["context_size"] = m.contextSize;
        item["zenith_meta"] = meta; // 扩展字段（OpenAI 客户端会忽略）
        data.push_back(std::move(item));
    }
    JsonValue root = JsonValue::object();
    root["object"] = "list";
    root["data"] = data;
    c.json(200, root);
}

// ---------------------------------------------------------------------------
//  /v1/chat/completions
// ---------------------------------------------------------------------------
JsonValue chatChunkBase(const std::string& id, int64_t created, const std::string& model) {
    JsonValue chunk = JsonValue::object();
    chunk["id"] = id;
    chunk["object"] = "chat.completion.chunk";
    chunk["created"] = created;
    chunk["model"] = model;
    return chunk;
}

void handleChat(http::Context& c, IInferenceService& svc, const OpenAiApiOptions& opt) {
    JsonValue body;
    if (!parseBody(c, body)) return;

    ChatRequest req;
    bool stream = false;
    std::string err;
    if (!buildChatRequest(body, req, stream, err)) {
        sendError(c, 400, "invalid_request_error", err);
        return;
    }
    countRequest(opt.metricsEnabled, "chat");

    const std::string id = "chatcmpl-" + randomHex(12);
    const int64_t created = nowSec();
    const std::string model = req.model.empty() && !svc.models().empty() ? svc.models().front().id
                                                                        : req.model;
    req.model = model;

    if (stream) {
        auto sse = c.sse(200);
        if (sse == nullptr) {
            sendError(c, 500, "server_error", "streaming is unavailable on this connection");
            return;
        }
        IInferenceService* service = &svc;
        const bool metricsOn = opt.metricsEnabled;
        const bool withUsage = opt.includeUsageInStream;
        runAsync(opt, [service, req, id, created, model, sse, metricsOn, withUsage]() {
            auto emitDelta = [&](std::string_view delta) {
                JsonValue chunk = chatChunkBase(id, created, model);
                JsonValue choices = JsonValue::array();
                JsonValue choice = JsonValue::object();
                choice["index"] = 0;
                JsonValue d = JsonValue::object();
                d["content"] = std::string(delta);
                choice["delta"] = d;
                choice["finish_reason"] = nullptr;
                choices.push_back(std::move(choice));
                chunk["choices"] = std::move(choices);
                sse->send(chunk.dump());
            };
            // 思考模型的推理过程单独成帧（llama.cpp 的 reasoning_content 语义）
            auto emitReasoning = [&](std::string_view delta) {
                JsonValue chunk = chatChunkBase(id, created, model);
                JsonValue choices = JsonValue::array();
                JsonValue choice = JsonValue::object();
                choice["index"] = 0;
                JsonValue d = JsonValue::object();
                d["reasoning_content"] = std::string(delta);
                choice["delta"] = d;
                choice["finish_reason"] = nullptr;
                choices.push_back(std::move(choice));
                chunk["choices"] = std::move(choices);
                sse->send(chunk.dump());
            };

            ChatResult r = service->chat(
                req,
                [&](std::string_view delta) {
                    if (sse->closed()) return false;
                    emitDelta(delta);
                    return true;
                },
                [&](std::string_view delta) {
                    if (sse->closed()) return false;
                    emitReasoning(delta);
                    return true;
                });

            if (!r.ok && !sse->closed()) {
                // 流已开启只能以 error 事件收尾（客户端会看到而非静默截断）
                JsonValue e = JsonValue::object();
                e["error"] = [&] {
                    JsonValue v = JsonValue::object();
                    v["message"] = r.error.empty() ? "inference failed" : r.error;
                    v["type"] = "server_error";
                    return v;
                }();
                sse->send(e.dump(), "error");
                sse->close();
                return;
            }

            // 收尾片：finish_reason 非 null，delta 为空
            JsonValue tail = chatChunkBase(id, created, model);
            JsonValue choices = JsonValue::array();
            JsonValue choice = JsonValue::object();
            choice["index"] = 0;
            choice["delta"] = JsonValue::object();
            choice["finish_reason"] = r.finishReason.empty() ? "stop" : r.finishReason;
            choices.push_back(std::move(choice));
            tail["choices"] = std::move(choices);
            sse->send(tail.dump());

            if (withUsage) {
                JsonValue usageChunk = chatChunkBase(id, created, model);
                usageChunk["choices"] = JsonValue::array();
                usageChunk["usage"] = usageObject(r.usage);
                sse->send(usageChunk.dump());
            }
            sse->send("[DONE]");
            sse->close();
            countTokens(metricsOn, r.usage);
            recordEngineStats(metricsOn, r.stats);
        });
        return;
    }

    // 非流式：有池时用 chunked 延迟响应（绝不在 EventLoop 上跑推理），无池就地序列化
    if (opt.pool != nullptr) {
        auto writer = c.chunkedStream("application/json; charset=utf-8");
        if (writer != nullptr) {
            IInferenceService* service = &svc;
            const bool metricsOn = opt.metricsEnabled;
            runAsync(opt, [service, req, id, created, model, writer, metricsOn]() {
                ChatResult r = service->chat(req, nullptr);
                JsonValue root = JsonValue::object();
                if (!r.ok) {
                    JsonValue e = JsonValue::object();
                    JsonValue inner = JsonValue::object();
                    inner["message"] = r.error.empty() ? "inference failed" : r.error;
                    inner["type"] = "server_error";
                    e["error"] = std::move(inner);
                    root = std::move(e);
                } else {
                    root["id"] = id;
                    root["object"] = "chat.completion";
                    root["created"] = created;
                    root["model"] = model;
                    JsonValue choices = JsonValue::array();
                    JsonValue choice = JsonValue::object();
                    choice["index"] = 0;
                    JsonValue message = JsonValue::object();
                    message["role"] = "assistant";
                    message["content"] = r.content;
                    if (!r.reasoning.empty()) message["reasoning_content"] = r.reasoning;
                    choice["message"] = std::move(message);
                    choice["finish_reason"] =
                        r.finishReason.empty() ? "stop" : r.finishReason;
                    choices.push_back(std::move(choice));
                    root["choices"] = std::move(choices);
                    root["usage"] = usageObject(r.usage);
                    countTokens(metricsOn, r.usage);
            recordEngineStats(metricsOn, r.stats);
                }
                writer->write(root.dump());
                writer->close();
            });
            return;
        }
    }

    ChatResult r = svc.chat(req, nullptr);
    if (!r.ok) {
        sendError(c, 500, "server_error", r.error.empty() ? "inference failed" : r.error);
        return;
    }
    countTokens(opt.metricsEnabled, r.usage);
    recordEngineStats(opt.metricsEnabled, r.stats);
    JsonValue root = JsonValue::object();
    root["id"] = id;
    root["object"] = "chat.completion";
    root["created"] = created;
    root["model"] = model;
    JsonValue choices = JsonValue::array();
    JsonValue choice = JsonValue::object();
    choice["index"] = 0;
    JsonValue message = JsonValue::object();
    message["role"] = "assistant";
    message["content"] = r.content;
    if (!r.reasoning.empty()) message["reasoning_content"] = r.reasoning;
    choice["message"] = std::move(message);
    choice["finish_reason"] = r.finishReason.empty() ? "stop" : r.finishReason;
    choices.push_back(std::move(choice));
    root["choices"] = std::move(choices);
    root["usage"] = usageObject(r.usage);
    c.json(200, root);
}

// ---------------------------------------------------------------------------
//  /v1/completions（文本补全：prompt → user 消息）
// ---------------------------------------------------------------------------
void handleCompletions(http::Context& c, IInferenceService& svc, const OpenAiApiOptions& opt) {
    JsonValue body;
    if (!parseBody(c, body)) return;

    const JsonValue& promptNode = body["prompt"];
    std::string prompt;
    if (promptNode.isString()) {
        prompt = promptNode.asString();
    } else if (const auto* list = promptNode.asArray(); list != nullptr && !list->empty()) {
        if ((*list)[0].isString()) prompt = (*list)[0].asString();
    }
    if (prompt.empty()) {
        sendError(c, 400, "invalid_request_error", "'prompt' is required");
        return;
    }
    countRequest(opt.metricsEnabled, "completion");

    ChatRequest req;
    req.model = body["model"].asString();
    req.messages.push_back(ChatMessage{"user", prompt, ""});
    if (body.has("max_tokens")) req.maxTokens = static_cast<int>(body["max_tokens"].asInt(512));
    if (body.has("temperature")) req.temperature = body["temperature"].asDouble(0.7);
    if (body.has("top_p")) req.topP = body["top_p"].asDouble(1.0);
    if (req.maxTokens <= 0) req.maxTokens = 512;

    const bool stream = body["stream"].asBool(false);
    const std::string id = "cmpl-" + randomHex(12);
    const int64_t created = nowSec();
    const std::string model = req.model.empty() && !svc.models().empty() ? svc.models().front().id
                                                                        : req.model;
    req.model = model;

    if (stream) {
        auto sse = c.sse(200);
        if (sse == nullptr) {
            sendError(c, 500, "server_error", "streaming is unavailable on this connection");
            return;
        }
        IInferenceService* service = &svc;
        const bool metricsOn = opt.metricsEnabled;
        runAsync(opt, [service, req, id, created, model, sse, metricsOn]() {
            ChatResult r = service->chat(req, [&](std::string_view delta) {
                if (sse->closed()) return false;
                JsonValue out = JsonValue::object();
                out["id"] = id;
                out["object"] = "text_completion";
                out["created"] = created;
                out["model"] = model;
                JsonValue choices = JsonValue::array();
                JsonValue choice = JsonValue::object();
                choice["index"] = 0;
                choice["text"] = std::string(delta);
                choice["finish_reason"] = nullptr;
                choices.push_back(std::move(choice));
                out["choices"] = std::move(choices);
                sse->send(out.dump());
                return true;
            });
            if (r.ok || !sse->closed()) {
                JsonValue tail = JsonValue::object();
                tail["id"] = id;
                tail["object"] = "text_completion";
                tail["created"] = created;
                tail["model"] = model;
                JsonValue choices = JsonValue::array();
                JsonValue choice = JsonValue::object();
                choice["index"] = 0;
                choice["text"] = "";
                choice["finish_reason"] = r.finishReason.empty() ? "stop" : r.finishReason;
                choices.push_back(std::move(choice));
                tail["choices"] = std::move(choices);
                sse->send(tail.dump());
                sse->send("[DONE]");
                sse->close();
                countTokens(metricsOn, r.usage);
            recordEngineStats(metricsOn, r.stats);
            } else {
                sse->close();
            }
        });
        return;
    }

    ChatResult r = svc.chat(req, nullptr);
    if (!r.ok) {
        sendError(c, 500, "server_error", r.error.empty() ? "inference failed" : r.error);
        return;
    }
    countTokens(opt.metricsEnabled, r.usage);
    recordEngineStats(opt.metricsEnabled, r.stats);
    JsonValue root = JsonValue::object();
    root["id"] = id;
    root["object"] = "text_completion";
    root["created"] = created;
    root["model"] = model;
    JsonValue choices = JsonValue::array();
    JsonValue choice = JsonValue::object();
    choice["index"] = 0;
    choice["text"] = r.content;
    choice["finish_reason"] = r.finishReason.empty() ? "stop" : r.finishReason;
    choices.push_back(std::move(choice));
    root["choices"] = std::move(choices);
    root["usage"] = usageObject(r.usage);
    c.json(200, root);
}

// ---------------------------------------------------------------------------
//  /v1/embeddings
// ---------------------------------------------------------------------------
void handleEmbeddings(http::Context& c, IInferenceService& svc, const OpenAiApiOptions& opt) {
    JsonValue body;
    if (!parseBody(c, body)) return;

    std::vector<std::string> inputs;
    const JsonValue& in = body["input"];
    if (in.isString()) {
        inputs.push_back(in.asString());
    } else if (const auto* list = in.asArray(); list != nullptr) {
        for (const auto& item : *list) {
            if (item.isString()) inputs.push_back(item.asString());
        }
    }
    if (inputs.empty()) {
        sendError(c, 400, "invalid_request_error", "'input' must be a string or array of strings");
        return;
    }
    if (inputs.size() > 256) {
        sendError(c, 400, "invalid_request_error", "'input' exceeds the 256-item limit");
        return;
    }
    countRequest(opt.metricsEnabled, "embedding");

    const std::string model = body["model"].asString();
    JsonValue data = JsonValue::array();
    Usage total;
    std::string firstError;
    for (size_t i = 0; i < inputs.size(); ++i) {
        EmbeddingResult er = svc.embeddings(model, inputs[i]);
        if (!er.ok) {
            if (firstError.empty()) firstError = er.error;
            continue;
        }
        total.promptTokens += er.usage.promptTokens;
        JsonValue item = JsonValue::object();
        item["object"] = "embedding";
        item["index"] = static_cast<int64_t>(i);
        JsonValue vec = JsonValue::array();
        for (float f : er.vector) vec.push_back(static_cast<double>(f));
        item["embedding"] = std::move(vec);
        data.push_back(std::move(item));
    }
    if (data.empty()) {
        sendError(c, 500, "server_error",
                  firstError.empty() ? "embedding failed" : firstError);
        return;
    }
    countTokens(opt.metricsEnabled, total);

    JsonValue root = JsonValue::object();
    root["object"] = "list";
    root["model"] = model;
    root["data"] = std::move(data);
    root["usage"] = usageObject(total);
    c.json(200, root);
}

// ---------------------------------------------------------------------------
//  /zenith/ai/status —— 网关与引擎运行态（不属 OpenAI 面）
// ---------------------------------------------------------------------------
void handleStatus(http::Context& c, IInferenceService& svc, const OpenAiApiOptions& opt) {
    const EngineState st = svc.engineState();
    recordEngineState(opt.metricsEnabled, st);

    JsonValue root = JsonValue::object();
    root["backend"] = svc.backend();

    JsonValue models = JsonValue::array();
    for (const auto& m : svc.models()) models.push_back(m.id);
    root["models"] = std::move(models);

    JsonValue engine = JsonValue::object();
    engine["supervised"] = st.supervised;
    engine["running"] = st.running;
    engine["pid"] = st.pid;
    engine["restarts"] = st.restarts;
    engine["requests"] = st.requests;
    engine["last_ttft_ms"] = st.lastTtftMs;
    engine["last_prefill_tps"] = st.lastPrefillTps;
    engine["last_decode_tps"] = st.lastDecodeTps;
    engine["draft_accepted"] = st.draftAccepted;
    engine["draft_total"] = st.draftTotal;
    engine["last_line"] = st.lastLine;
    root["engine"] = std::move(engine);

    auto& reg = metrics::Registry::instance();
    JsonValue counters = JsonValue::object();
    counters["requests_total"] = reg.counter("zenith_ai_requests_total").value();
    counters["prompt_tokens_total"] = reg.counter("zenith_ai_prompt_tokens_total").value();
    counters["completion_tokens_total"] = reg.counter("zenith_ai_completion_tokens_total").value();
    root["counters"] = std::move(counters);

    c.json(200, root);
}

} // namespace

void mountOpenAiApi(http::Router& router, IInferenceService& service,
                    const OpenAiApiOptions& options) {
    const std::string p = options.prefix.empty() ? std::string("/v1") : options.prefix;

    router.get(p + "/models", [&service, options](http::Context& c) {
        handleModels(c, service, options);
    });
    router.post(p + "/chat/completions", [&service, options](http::Context& c) {
        handleChat(c, service, options);
    });
    router.post(p + "/completions", [&service, options](http::Context& c) {
        handleCompletions(c, service, options);
    });
    router.post(p + "/embeddings", [&service, options](http::Context& c) {
        handleEmbeddings(c, service, options);
    });
    // 网关/引擎运行态（放在 /v1 之外，避免污染 OpenAI 兼容面）
    router.get("/zenith/ai/status", [&service, options](http::Context& c) {
        handleStatus(c, service, options);
    });
}

} // namespace zenith::ai
