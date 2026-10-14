// ai_comfyui 插件测试：mock ComfyUI（Zenith 自家 HttpServer）全链路
// 流程：插件加载 → draw(workflow) → mock 收 /prompt → /history 完成 → /view 出图 → 断言产物
#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

#include "ai_service.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
/// mock ComfyUI：收 /prompt 后两轮 /history 报 completed，/view 回 PNG 魔数图
struct MockComfy {
    net::EventLoop loop;
    net::EventLoopThreadPool io{1};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    std::atomic<int> promptsReceived{0};
    std::string lastWorkflow;
    std::atomic<int> historyPolls{0};
    std::string promptId = "mock-pid-0001";

    MockComfy() {
        io.start();
        server.router().post("/prompt", [this](http::Context& c) {
            lastWorkflow = c.body();
            promptsReceived.fetch_add(1);
            http::json::JsonValue v = http::json::JsonValue::object();
            v["prompt_id"] = promptId;
            c.json(200, v);
        });
        server.router().get("/history/:id", [this](http::Context& c) {
            const int polls = historyPolls.fetch_add(1);
            http::json::JsonValue v = http::json::JsonValue::object();
            if (polls < 1) {
                // 首轮：执行中
                v[promptId] = [] {
                    http::json::JsonValue e = http::json::JsonValue::object();
                    e["status"] = [] {
                        http::json::JsonValue st = http::json::JsonValue::object();
                        st["completed"] = false;
                        return st;
                    }();
                    return e;
                }();
            } else {
                v[promptId] = [] {
                    http::json::JsonValue e = http::json::JsonValue::object();
                    e["status"] = [] {
                        http::json::JsonValue st = http::json::JsonValue::object();
                        st["completed"] = true;
                        return st;
                    }();
                    e["outputs"] = [] {
                        http::json::JsonValue o = http::json::JsonValue::object();
                        o["9"] = [] {
                            http::json::JsonValue imgs = http::json::JsonValue::object();
                            http::json::JsonValue arr = http::json::JsonValue::array();
                            http::json::JsonValue img = http::json::JsonValue::object();
                            img["filename"] = "zenith_mock_output.png";
                            img["type"] = "output";
                            arr.push_back(img);
                            imgs["images"] = arr;
                            return imgs;
                        }();
                        return o;
                    }();
                    return e;
                }();
            }
            c.json(200, v);
        });
        server.router().get("/view", [this](http::Context& c) {
            (void)this;
            const std::string file = c.query("filename");
            (void)file;
            // PNG 魔数 + 填充
            std::string png("\x89PNG\r\n\x1a\n", 8);
            png.append(256, 'Z');
            c.setHeader("Content-Type", "image/png");
            c.status(200);
            c.response().body = std::move(png);
        });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(120ms);
    }
    ~MockComfy() {
        loop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }
};
} // namespace

ZTEST(AiPlugin, DrawFullChainViaMockComfy) {
    MockComfy mock;

    core::KeyValueConfig cfg;
    cfg.set("ai.comfyui.host", "127.0.0.1");
    cfg.set("ai.comfyui.port", std::to_string(mock.port()));

    concurrency::ThreadPool pool(1);
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, &cfg);
    (void)mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);

    void* raw = registry.queryRaw(ai::IAiDrawService::service_name(),
                                   ai::IAiDrawService::service_version());
    ZEXPECT_TRUE(raw != nullptr);
    if (raw == nullptr) return;
    auto* svc = static_cast<ai::IAiDrawService*>(raw);

    const std::string dir = "zai.tmp";
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);

    // workflow：占位符 {{seed}} 应被随机化、{{style}} 经 resolver 注入
    const std::string wf = R"({"3":{"inputs":{"seed":{{seed}},"style":"{{style}}"}}})";
    auto result = svc->draw(wf, dir, [](const std::string& k) {
        return k == "style" ? std::string("ink-wash") : std::string();
    });

    ZEXPECT_TRUE(result.ok);
    ZEXPECT_EQ(result.promptId, std::string("mock-pid-0001"));
    ZEXPECT_EQ(result.outputs.size(), size_t{1});
    if (!result.outputs.empty()) {
        ZEXPECT_TRUE(std::filesystem::exists(result.outputs[0]));
        ZEXPECT_TRUE(std::filesystem::file_size(result.outputs[0]) >= 264); // 8 魔数 + 256
        // PNG 魔数
        FILE* f = std::fopen(result.outputs[0].c_str(), "rb");
        if (f != nullptr) {
            unsigned char magic[8] = {0};
            std::fread(magic, 1, 8, f);
            std::fclose(f);
            ZEXPECT_EQ(magic[0], 0x89);
            ZEXPECT_EQ(magic[1], 0x50); // 'P'
        }
    }
    ZEXPECT_TRUE(result.error.empty());

    // mock 侧验证：workflow 已收到且占位符已注入
    ZEXPECT_EQ(mock.promptsReceived.load(), 1);
    ZEXPECT_TRUE(mock.lastWorkflow.find("{{") == std::string::npos); // 无残留占位符
    ZEXPECT_TRUE(mock.lastWorkflow.find("ink-wash") != std::string::npos);
    ZEXPECT_TRUE(mock.lastWorkflow.find("\"seed\":") != std::string::npos);
    ZEXPECT_TRUE(mock.historyPolls.load() >= 2);

    mgr.unloadAll();
    std::filesystem::remove_all(dir, ec);
}

ZTEST(AiPlugin, UnreachableServerFails) {
    core::KeyValueConfig cfg;
    cfg.set("ai.comfyui.host", "127.0.0.1");
    cfg.set("ai.comfyui.port", "1"); // 不可达

    concurrency::ThreadPool pool(1);
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, &cfg);
    (void)mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);
    void* raw = registry.queryRaw(ai::IAiDrawService::service_name(), 1);
    ZEXPECT_TRUE(raw != nullptr);
    if (raw == nullptr) return;
    auto* svc = static_cast<ai::IAiDrawService*>(raw);

    auto r = svc->draw("{}", ".", nullptr, 3);
    ZEXPECT_FALSE(r.ok);
    ZEXPECT_FALSE(r.error.empty());
}

ZTEST_MAIN
