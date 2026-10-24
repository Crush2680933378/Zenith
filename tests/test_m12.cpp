// M12 测试：多厂商 LLM 路由 —— 3 个 mock 厂商（OpenAI/Anthropic/Gemini 形态）
// 用 Zenith 自家 HttpServer 扮演（吃自家狗粮），验证协议适配/路由/发现/嵌入
#include <cmath>
#include <cstdlib>
#include <thread>

#include "ztest.hpp"

#include "zenith/ai/provider_router.hpp"
#include "zenith/http/http_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {

/// 三协议 mock 厂商各起一个端口
struct MockVendors {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    http::HttpServer openai{&loop, &io};      // /v1/chat/completions + /v1/models + /v1/embeddings
    http::HttpServer anthropic{&loop, &io};   // /v1/messages
    http::HttpServer gemini{&loop, &io};      // /models/{m}:generateContent
    std::thread runner;
    std::string openaiAuth, anthropicAuth, geminiAuth; // 记录收到的鉴权头

    MockVendors() {
        io.start();

        // ---- OpenAI 形态（DeepSeek 同构）----
        openai.router().post("/chat/completions", [this](http::Context& c) {
            openaiAuth = c.header("Authorization");
            auto b = c.bodyJson();
            json::JsonValue out = json::JsonValue::object();
            out["id"] = "chatcmpl-mock";
            out["model"] = b["model"].asString();
            json::JsonValue choice = json::JsonValue::object();
            choice["message"]["role"] = "assistant";
            choice["message"]["content"] = "[" + b["model"].asString() + "] " +
                                           b["messages"].asArray()->at(size_t{0})["content"].asString();
            choice["finish_reason"] = "stop";
            json::JsonValue choices = json::JsonValue::array();
            choices.push_back(std::move(choice));
            out["choices"] = std::move(choices);
            out["usage"]["prompt_tokens"] = int64_t{5};
            out["usage"]["completion_tokens"] = int64_t{7};
            c.json(200, out);
        });
        openai.router().get("/models", [](http::Context& c) {
            json::JsonValue out = json::JsonValue::object();
            json::JsonValue data = json::JsonValue::array();
            for (const char* m : {"deepseek-chat", "deepseek-reasoner", "kimi-k2"}) {
                json::JsonValue e = json::JsonValue::object();
                e["id"] = m;
                data.push_back(std::move(e));
            }
            out["data"] = std::move(data);
            c.json(200, out);
        });
        openai.router().post("/embeddings", [](http::Context& c) {
            json::JsonValue out = json::JsonValue::object();
            json::JsonValue arr = json::JsonValue::array();
            json::JsonValue e = json::JsonValue::object();
            json::JsonValue vec = json::JsonValue::array();
            vec.push_back(0.1);
            vec.push_back(0.2);
            vec.push_back(0.3);
            e["embedding"] = std::move(vec);
            arr.push_back(std::move(e));
            out["data"] = std::move(arr);
            c.json(200, out);
        });

        // ---- Anthropic 形态 ----
        anthropic.router().post("/v1/messages", [this](http::Context& c) {
            anthropicAuth = c.header("x-api-key");
            anthropicAuth += "|" + c.header("anthropic-version");
            auto b = c.bodyJson();
            // 校验 system 提取 + max_tokens 必在
            (void)b;
            json::JsonValue out = json::JsonValue::object();
            json::JsonValue blk = json::JsonValue::object();
            blk["type"] = "text";
            blk["text"] = "[" + b["model"].asString() + "] via anthropic";
            json::JsonValue content = json::JsonValue::array();
            content.push_back(std::move(blk));
            out["content"] = std::move(content);
            out["stop_reason"] = "end_turn";
            out["usage"]["input_tokens"] = int64_t{11};
            out["usage"]["output_tokens"] = int64_t{13};
            c.json(200, out);
        });

        // ---- Gemini 形态（/models/{model}:generateContent —— 冒号动作在同一段）----
        gemini.router().post("/models/:gen", [this](http::Context& c) {
            const std::string seg = c.param("gen"); // "gemini-x:generateContent"
            const size_t colon = seg.rfind(':');
            const std::string action =
                colon == std::string::npos ? "" : seg.substr(colon + 1);
            const std::string model =
                colon == std::string::npos ? seg : seg.substr(0, colon);
            geminiAuth = c.header("x-goog-api-key");
            geminiAuth += "|model=" + model;
            auto b = c.bodyJson();
            (void)b;
            json::JsonValue out = json::JsonValue::object();
            json::JsonValue cand = json::JsonValue::object();
            json::JsonValue part = json::JsonValue::object();
            part["text"] = "[" + model + "] via gemini";
            json::JsonValue parts = json::JsonValue::array();
            parts.push_back(std::move(part));
            cand["content"]["parts"] = std::move(parts);
            cand["finishReason"] = "STOP";
            json::JsonValue cands = json::JsonValue::array();
            cands.push_back(std::move(cand));
            out["candidates"] = std::move(cands);
            out["usageMetadata"]["promptTokenCount"] = int64_t{17};
            out["usageMetadata"]["candidatesTokenCount"] = int64_t{19};
            c.json(200, out);
        });

        (void)openai.listen("127.0.0.1", 0);
        (void)anthropic.listen("127.0.0.1", 0);
        (void)gemini.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(150ms);
    }
    ~MockVendors() {
        loop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t openaiPort() const { return openai.port(); }
    [[nodiscard]] uint16_t anthropicPort() const { return anthropic.port(); }
    [[nodiscard]] uint16_t geminiPort() const { return gemini.port(); }
};

ai::RouterConfig makeConfig(const MockVendors& mv) {
    const std::string cfgJson = R"({
      "providers": [
        {"name":"deepseek","api":"openai-chat","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.openaiPort()) + R"(","apiKey":"sk-ds-test",
         "models":["deepseek-chat","deepseek-reasoner"]},
        {"name":"anthropic","api":"anthropic","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.anthropicPort()) + R"(","apiKey":"sk-ant-test",
         "models":[]},
        {"name":"google","api":"gemini","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.geminiPort()) + R"(","apiKey":"AIza-test",
         "models":[]}
      ],
      "routes": {"claude-*":"anthropic","gemini-*":"google","kimi-*":"deepseek"},
      "defaultProvider": "deepseek"
    })";
    auto v = json::JsonValue::parse(cfgJson);
    auto cfg = ai::RouterConfig::fromJson(v.value());
    return std::move(cfg).value();
}

ai::ChatRequest makeReq(const std::string& model) {
    ai::ChatRequest r;
    r.model = model;
    r.maxTokens = 1024;
    ai::ChatMessage sys;
    sys.role = "system";
    sys.content = "be brief";
    r.messages.push_back(std::move(sys));
    ai::ChatMessage u;
    u.role = "user";
    u.content = "hi";
    r.messages.push_back(std::move(u));
    return r;
}

} // namespace

ZTEST(Router, RoutesAllThreeProtocols) {
    MockVendors mv;
    ai::ProviderRouter router(makeConfig(mv));
    ZEXPECT_EQ(router.providerCount(), size_t{3});

    // 精确匹配 → deepseek（openai-chat）
    auto r1 = router.chat(makeReq("deepseek-chat"));
    ZEXPECT_TRUE(r1.ok);
    ZEXPECT_TRUE(r1.content.find("[deepseek-chat]") != std::string::npos);
    ZEXPECT_EQ(mv.openaiAuth, std::string("Bearer sk-ds-test"));
    ZEXPECT_EQ(r1.usage.promptTokens, 5);

    // 通配 claude-* → anthropic（system 提取/x-api-key/anthropic-version）
    auto r2 = router.chat(makeReq("claude-sonnet-4-5"));
    ZEXPECT_TRUE(r2.ok);
    ZEXPECT_TRUE(r2.content.find("[claude-sonnet-4-5] via anthropic") != std::string::npos);
    ZEXPECT_EQ(mv.anthropicAuth, std::string("sk-ant-test|2023-06-01"));
    ZEXPECT_EQ(r2.usage.promptTokens, 11);

    // 通配 gemini-* → google（systemInstruction/role 映射/x-goog-api-key）
    auto r3 = router.chat(makeReq("gemini-2.5-pro"));
    ZEXPECT_TRUE(r3.ok);
    ZEXPECT_TRUE(r3.content.find("[gemini-2.5-pro] via gemini") != std::string::npos);
    ZEXPECT_EQ(mv.geminiAuth, std::string("AIza-test|model=gemini-2.5-pro"));

    // 默认厂商兜底（未知模型 → defaultProvider=deepseek）
    auto r4 = router.chat(makeReq("unknown-model"));
    ZEXPECT_TRUE(r4.ok);
    ZEXPECT_TRUE(r4.content.find("[unknown-model]") != std::string::npos); // 兜底走了 deepseek
    ZEXPECT_EQ(ai::ProviderRouter::lastProviderUsed, std::string("deepseek"));

    // 路由诊断
    ZEXPECT_EQ(router.route("deepseek-reasoner")->name, std::string("deepseek"));
    ZEXPECT_EQ(router.route("claude-x")->name, std::string("anthropic"));
}

ZTEST(Router, DiscoveryAndEmbeddings) {
    MockVendors mv;
    ai::ProviderRouter router(makeConfig(mv));

    // openai 协议发现（GET /models → data[].id）
    auto found = router.discoverModels("deepseek");
    ZEXPECT_TRUE(static_cast<bool>(found));
    ZEXPECT_EQ(found.value().size(), size_t{3});
    ZEXPECT_EQ(found.value()[0], std::string("deepseek-chat"));

    // 非 openai 协议 → 返回配置内清单（与 DeepSeek Harness 同取舍）
    auto anthropicList = router.discoverModels("anthropic");
    ZEXPECT_TRUE(static_cast<bool>(anthropicList));
    ZEXPECT_EQ(anthropicList.value().size(), size_t{0});

    // 嵌入（openai 协议专属）
    auto e = router.embeddings("deepseek-chat", "hello");
    ZEXPECT_TRUE(e.ok);
    ZEXPECT_EQ(e.vector.size(), size_t{3});
    ZEXPECT_TRUE(std::abs(e.vector[1] - 0.2) < 1e-6);

    // 嵌入路由到非 openai 协议 → 明确报错
    auto bad = router.embeddings("claude-x", "hello");
    ZEXPECT_FALSE(bad.ok);
}

ZTEST(Router, ConfigErrorsAndEnvKey) {
    MockVendors mv;
    // 坏 JSON 键：providers 缺失
    auto c1 = ai::RouterConfig::fromJson(
        json::JsonValue::parse(R"({"foo":1})").value());
    ZEXPECT_TRUE(!c1);

    // 单条 provider 非法 api → 跳过；合法的保留
    auto c2 = ai::RouterConfig::fromJson(json::JsonValue::parse(R"({
      "providers": [
        {"name":"bad","api":"nope","baseURL":"http://x","apiKey":"k","models":[]},
        {"name":"good","api":"openai-chat","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.openaiPort()) + R"(","apiKey":"$ENV:ZTEST_ROUTER_KEY","models":["m"]}
      ]})").value());
    ZEXPECT_TRUE(static_cast<bool>(c2));
    ZEXPECT_EQ(c2.value().providers.size(), size_t{1});
#ifdef _WIN32
    _putenv_s("ZTEST_ROUTER_KEY", "sk-env-resolved");
#else
    setenv("ZTEST_ROUTER_KEY", "sk-env-resolved", 1);
#endif
    // ENV 解析发生在 fromJson 时——重新解析
    auto c3 = ai::RouterConfig::fromJson(json::JsonValue::parse(R"({
      "providers": [
        {"name":"good","api":"openai-chat","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.openaiPort()) + R"(","apiKey":"$ENV:ZTEST_ROUTER_KEY","models":["m"]}
      ]})").value());
    ZEXPECT_EQ(c3.value().providers[0].apiKey, std::string("sk-env-resolved"));
    ai::ProviderRouter r3(std::move(c3).value());
    auto chat = r3.chat(makeReq("m"));
    ZEXPECT_TRUE(chat.ok);
    ZEXPECT_EQ(mv.openaiAuth, std::string("Bearer sk-env-resolved"));
}

ZTEST(Router, NoRouteError) {
    MockVendors mv;
    // 无 defaultProvider 且无通配 → 未知模型明确报错
    auto cfg = json::JsonValue::parse(R"({
      "providers": [
        {"name":"deepseek","api":"openai-chat","baseURL":"http://127.0.0.1:)" +
        std::to_string(mv.openaiPort()) + R"(","apiKey":"k","models":["deepseek-chat"]}
      ]})").value();
    ai::ProviderRouter r(ai::RouterConfig::fromJson(cfg).value());
    auto bad = r.chat(makeReq("gpt-99"));
    ZEXPECT_FALSE(bad.ok);
    ZEXPECT_TRUE(bad.error.find("no provider routes") != std::string::npos);
    // 已知模型仍通
    auto ok = r.chat(makeReq("deepseek-chat"));
    ZEXPECT_TRUE(ok.ok);
}

ZTEST_MAIN
