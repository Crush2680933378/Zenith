// ============================================================================
//  插件热重载测试（M8.2）：.new 交换协议 + 失败安全
//  用 ai_inference 插件（mock 后端，重载零成本）验证机制本身。
// ============================================================================
#include <cstdio>
#include <filesystem>
#include <string>

#include "ztest.hpp"

#include "zenith/ai/inference_service.hpp"
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/plugin_watcher.hpp"
#include "zenith/plugin/service_registry.hpp"

using namespace zenith;

namespace fs = std::filesystem;

namespace {

constexpr const char* kPluginName = "zenith.ai.IInferenceService";
const std::string kDll = std::string("zenith_ai_inference") + plugin::pluginFileExtension();
const char* kDir = "zreload.tmp";

/// 准备一个只放 ai_inference 的临时插件目录
bool prepareDir() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
    fs::create_directories(kDir, ec);
    const fs::path src = fs::path(ZENITH_TEST_PLUGIN_DIR) / kDll;
    if (!fs::exists(src, ec)) return false;
    fs::copy_file(src, fs::path(kDir) / kDll, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

struct Fixture {
    concurrency::ThreadPool pool{1};
    plugin::ServiceRegistry registry;
    core::KeyValueConfig cfg;
    plugin::PluginManager mgr{registry, &pool, &cfg};

    Fixture() {
        cfg.set("ai.inference.backend", "mock");
        cfg.set("ai.inference.default_model", "reload-model");
        auto r = mgr.loadDirectory(kDir);
        ZEXPECT_TRUE(r.isOk());
        (void)mgr.startAll();
    }
    ~Fixture() {
        mgr.unloadAll();
        std::error_code ec;
        fs::remove_all(kDir, ec); // 测试产生的临时插件目录不留在构建树里
    }

    [[nodiscard]] ai::IInferenceService* service() {
        return static_cast<ai::IInferenceService*>(registry.queryRaw(kPluginName, 1));
    }
};

} // namespace

ZTEST(PluginReload, NewFileSwapReplacesService) {
    if (!prepareDir()) {
        std::printf("[ SKIP ] %s 不在插件目录（未构建插件？）\n", kDll.c_str());
        return;
    }
    Fixture f;

    auto* svc1 = f.service();
    ZEXPECT_TRUE(svc1 != nullptr);
    if (svc1 == nullptr) return;
    ZEXPECT_EQ(svc1->backend(), std::string("mock"));
    ZEXPECT_TRUE(!svc1->models().empty());
    std::printf("[ reload ] 初次加载：backend=%s models=%zu\n", svc1->backend().c_str(),
                svc1->models().size());

    // 落一个 .new（真实场景里这是新编译出的 DLL）
    std::error_code ec;
    const fs::path dll = fs::path(kDir) / kDll;
    fs::copy_file(dll, fs::path(kDir) / (kDll + ".new"),
                  fs::copy_options::overwrite_existing, ec);
    ZEXPECT_FALSE(static_cast<bool>(ec));

    plugin::PluginWatcher watcher(f.mgr, kDir);
    std::string lastMsg;
    bool lastOk = false;
    watcher.onEvent([&](const plugin::PluginWatcher::Event& e) {
        lastOk = e.ok;
        lastMsg = e.message;
    });

    const size_t swapped = watcher.pollOnce();
    std::printf("[ reload ] pollOnce 重载数=%zu 事件 ok=%d msg=%s\n", swapped, lastOk ? 1 : 0,
                lastMsg.c_str());
    ZEXPECT_EQ(swapped, static_cast<size_t>(1));
    ZEXPECT_TRUE(lastOk);

    // 服务已被重新注册：旧指针不敢再用，重新查询
    auto* svc2 = f.service();
    ZEXPECT_TRUE(svc2 != nullptr);
    if (svc2 != nullptr) {
        ZEXPECT_EQ(svc2->backend(), std::string("mock"));
        ZEXPECT_TRUE(!svc2->models().empty());
        std::printf("[ reload ] 重载后：backend=%s models=%zu（指针 %p -> %p）\n",
                    svc2->backend().c_str(), svc2->models().size(),
                    static_cast<void*>(svc1), static_cast<void*>(svc2));
    }

    // 插件状态应为 Started，且目录里不该残留 .new / .old
    auto* info = f.mgr.find(kDll);
    ZEXPECT_TRUE(info != nullptr);
    if (info != nullptr) ZEXPECT_TRUE(info->state == plugin::PluginState::Started);
    ZEXPECT_FALSE(fs::exists(fs::path(kDir) / (kDll + ".new"), ec));
    ZEXPECT_FALSE(fs::exists(fs::path(kDir) / (std::string(kDll) + ".old"), ec));
}

ZTEST(PluginReload, BadNewFileDoesNotCrashHost) {
    if (!prepareDir()) {
        std::printf("[ SKIP ] %s 不在插件目录\n", kDll.c_str());
        return;
    }
    Fixture f;
    ZEXPECT_TRUE(f.service() != nullptr);

    // 故意写一个不是插件的文件
    {
        FILE* fp = std::fopen((std::string(kDir) + "/" + kDll + ".new").c_str(), "wb");
        ZEXPECT_TRUE(fp != nullptr);
        if (fp != nullptr) {
            const char junk[] = "not a plugin";
            std::fwrite(junk, 1, sizeof(junk) - 1, fp);
            std::fclose(fp);
        }
    }

    plugin::PluginWatcher watcher(f.mgr, kDir);
    bool sawFailure = false;
    watcher.onEvent([&](const plugin::PluginWatcher::Event& e) { sawFailure = !e.ok; });
    const size_t swapped = watcher.pollOnce();

    std::printf("[ reload ] 坏文件：重载数=%zu（期望 0），收到失败事件=%d\n", swapped,
                sawFailure ? 1 : 0);
    ZEXPECT_EQ(swapped, static_cast<size_t>(0));
    ZEXPECT_TRUE(sawFailure);
    // 宿主仍活着：注册表里已没有该服务（旧插件已卸载），但不会崩
    ZEXPECT_TRUE(f.service() == nullptr);
}

// ---------------------------------------------------------------------------
//  真模型热重载：ai_llama（GGUF）。验证"卸载会释放模型、重载会重新加载"。
//  模型缺失时 SKIP。
// ---------------------------------------------------------------------------
ZTEST(PluginReload, AiLlamaModelReloads) {
    const char* env = std::getenv("ZENITH_TEST_LLAMA_MODEL");
    std::string model = (env != nullptr && *env != '\0') ? std::string(env)
                                                         : std::string("D:\\App\\Qwen3.8-27B-UD-IQ3_XXS.gguf");
    if (!fs::exists(model)) {
        std::printf("[ SKIP ] 无 GGUF 模型：%s\n", model.c_str());
        return;
    }

    const std::string kLlamaDll = std::string("zenith_ai_llama") + plugin::pluginFileExtension();
    const char* kLlamaDir = "zreload-llama.tmp";
    std::error_code ec;
    fs::remove_all(kLlamaDir, ec);
    fs::create_directories(kLlamaDir, ec);
    const fs::path src = fs::path(ZENITH_TEST_PLUGIN_DIR) / kLlamaDll;
    if (!fs::exists(src, ec)) {
        std::printf("[ SKIP ] %s 未构建\n", kLlamaDll.c_str());
        return;
    }
    fs::copy_file(src, fs::path(kLlamaDir) / kLlamaDll, fs::copy_options::overwrite_existing, ec);
    ZEXPECT_FALSE(static_cast<bool>(ec));

    concurrency::ThreadPool pool{1};
    plugin::ServiceRegistry registry;
    core::KeyValueConfig cfg;
    cfg.set("ai.llama.model", model);
    cfg.set("ai.llama.context", "1024");
    cfg.set("ai.llama.threads", "8");
    cfg.set("ai.llama.model_id", "reload-llama");

    plugin::PluginManager mgr(registry, &pool, &cfg);
    auto r = mgr.loadDirectory(kLlamaDir);
    ZEXPECT_TRUE(r.isOk());
    (void)mgr.startAll();

    auto chatOnce = [&]() {
        auto* svc = static_cast<ai::IInferenceService*>(registry.queryRaw("zenith.ai.ILlamaInference", 1));
        if (svc == nullptr) return false;
        ai::ChatRequest req;
        req.messages.push_back(ai::ChatMessage{"user", "hi", ""});
        req.maxTokens = 8;
        req.temperature = 0.0;
        const auto res = svc->chat(req, nullptr, nullptr);
        return res.ok && !res.content.empty();
    };

    ZEXPECT_TRUE(chatOnce());
    std::printf("[ reload ] llama 初次加载并生成：OK\n");

    fs::copy_file(fs::path(kLlamaDir) / kLlamaDll,
                  fs::path(kLlamaDir) / (kLlamaDll + ".new"),
                  fs::copy_options::overwrite_existing, ec);
    plugin::PluginWatcher watcher(mgr, kLlamaDir);
    const size_t n = watcher.pollOnce();
    std::printf("[ reload ] llama 热重载数=%zu\n", n);
    ZEXPECT_EQ(n, static_cast<size_t>(1));
    ZEXPECT_TRUE(chatOnce()); // 重载后新实例仍可推理
    std::printf("[ reload ] llama 重载后生成：OK\n");

    mgr.unloadAll();
    fs::remove_all(kLlamaDir, ec);
}

ZTEST_MAIN
