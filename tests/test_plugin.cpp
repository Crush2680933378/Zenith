// plugin 模块单测：DynLib / ServiceRegistry / HostApi / PluginManager(e2e)
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/core/key_value_config.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/plugin/host_api.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service.hpp"
#include "zenith/plugin/service_registry.hpp"

#include "greeter.hpp"

using namespace zenith;

/* ---------------- 测试用服务 ---------------- */

namespace {

class ICounter : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(ICounter, "zenith.test.ICounter", 2)
    virtual int value() const = 0;
};

class CounterImpl final : public ICounter {
public:
    explicit CounterImpl(int v) : v_(v) {}
    [[nodiscard]] int value() const override { return v_; }

private:
    int v_;
};

} // namespace

/* ---------------- DynLib ---------------- */

ZTEST(DynLib, LoadMissingFileFails) {
    plugin::DynLib lib("no_such_plugin_xyz.dll");
    auto r = lib.load();
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_FALSE(lib.lastError().empty());
    ZEXPECT_EQ(lib.symbol("zenith_plugin_entry"), nullptr);
    ZEXPECT_FALSE(lib.loaded());
}

/* ---------------- ServiceRegistry ---------------- */

ZTEST(ServiceRegistry, TypedPutGetReplace) {
    plugin::ServiceRegistry reg;
    auto impl = std::make_shared<CounterImpl>(7);
    ZEXPECT_EQ(reg.put(impl), ZENITH_OK);

    auto got = reg.get<ICounter>();
    ZEXPECT_TRUE(got != nullptr);
    ZEXPECT_EQ(got->value(), 7);
    ZEXPECT_EQ(got.get(), impl.get());

    // 版本协商：所需版本高于实现 → 空
    ZEXPECT_TRUE(reg.get<ICounter>(99) == nullptr);
    // 重复注册 → ALREADY_EXISTS；replace 覆盖成功
    ZEXPECT_EQ(reg.put(std::make_shared<CounterImpl>(9)), ZENITH_ERR_ALREADY_EXISTS);
    ZEXPECT_EQ(reg.put(std::make_shared<CounterImpl>(9), /*replace=*/true), ZENITH_OK);
    ZEXPECT_EQ(reg.get<ICounter>()->value(), 9);
    ZEXPECT_EQ(reg.size(), size_t{1});
}

ZTEST(ServiceRegistry, RawRegisterQueryOwner) {
    plugin::ServiceRegistry reg;
    int instance = 0;
    int ownerA = 0, ownerB = 0;

    zenith_status st = ZENITH_OK;
    ZEXPECT_EQ(reg.queryRaw("nope", 1, &st), nullptr);
    ZEXPECT_EQ(st, ZENITH_ERR_NOT_FOUND);

    ZEXPECT_EQ(reg.registerRaw("svc.raw", 3, &instance, nullptr, &ownerA), ZENITH_OK);
    ZEXPECT_EQ(reg.queryRaw("svc.raw", 3, &st), static_cast<void*>(&instance));
    ZEXPECT_EQ(st, ZENITH_OK);
    // 版本不足
    ZEXPECT_EQ(reg.queryRaw("svc.raw", 4, &st), nullptr);
    ZEXPECT_EQ(st, ZENITH_ERR_VERSION_MISMATCH);

    // owner 清理：ownerB 未注册任何服务
    ZEXPECT_EQ(reg.removeByOwner(&ownerB), size_t{0});
    ZEXPECT_EQ(reg.removeByOwner(&ownerA), size_t{1});
    ZEXPECT_EQ(reg.queryRaw("svc.raw", 1, &st), nullptr);
}

ZTEST(ServiceRegistry, RemoveByOwnerCallsDestroy) {
    plugin::ServiceRegistry reg;
    static std::atomic<int> destroyed{0};
    destroyed = 0;

    auto* inst = new CounterImpl(1);
    void* owner = &reg;
    auto destroy = [](void* p) {
        delete static_cast<CounterImpl*>(p);
        destroyed.fetch_add(1);
    };
    ZEXPECT_EQ(reg.registerRaw("svc.dtor", 1, inst, destroy, owner), ZENITH_OK);
    ZEXPECT_EQ(reg.removeByOwner(owner), size_t{1});
    ZEXPECT_EQ(destroyed.load(), 1);
}

/* ---------------- HostApi（C thunk 适配层） ---------------- */

ZTEST(HostApi, PostTaskRunsOnPool) {
    concurrency::ThreadPool pool(2);
    plugin::ServiceRegistry reg;
    core::KeyValueConfig cfg;

    zenith_host_api api = plugin::makeHostApiV1(
        plugin::HostContext{&reg, &pool, &cfg}, /*ownerToken=*/nullptr);

    std::atomic<int> ran{0};
    // C 风格 fn/arg 转发
    struct Ctx { std::atomic<int>* ran; } ctx{&ran};
    api.post_task(api.pool_ctx,
                  [](void* p) { static_cast<Ctx*>(p)->ran->fetch_add(1); }, &ctx);

    for (int i = 0; i < 100 && ran.load() == 0; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ZEXPECT_EQ(ran.load(), 1);
}

ZTEST(HostApi, ConfigGetFallsBackToDefault) {
    concurrency::ThreadPool pool(1);
    plugin::ServiceRegistry reg;
    core::KeyValueConfig cfg;
    cfg.set("test.key", "value1");

    zenith_host_api api = plugin::makeHostApiV1(
        plugin::HostContext{&reg, &pool, &cfg}, nullptr);

    const char* out = nullptr;
    ZEXPECT_EQ(api.config_get(api.config_ctx, "test.key", "def", &out), ZENITH_OK);
    ZEXPECT_EQ(std::string(out), std::string("value1"));

    ZEXPECT_EQ(api.config_get(api.config_ctx, "absent", "def", &out), ZENITH_OK);
    ZEXPECT_EQ(std::string(out), std::string("def"));
}

ZTEST(HostApi, RegisterQueryThroughC) {
    plugin::ServiceRegistry reg;
    static int ownerDummy = 0; // 任意稳定指针作 owner 令牌
    zenith_host_api api = plugin::makeHostApiV1(
        plugin::HostContext{&reg, nullptr, nullptr}, &ownerDummy);

    CounterImpl inst(5);
    ZEXPECT_EQ(api.register_service(api.registry_ctx, api.plugin_token,
                                    ICounter::service_name(), ICounter::service_version(),
                                    &inst, nullptr),
               ZENITH_OK);
    void* out = nullptr;
    ZEXPECT_EQ(api.query_service(api.registry_ctx, ICounter::service_name(),
                                 ICounter::service_version(), &out),
               ZENITH_OK);
    ZEXPECT_EQ(static_cast<ICounter*>(out)->value(), 5);
    ZEXPECT_EQ(reg.removeByOwner(api.plugin_token), size_t{1});
}

/* ---------------- PluginManager 端到端（真实加载示例插件） ---------------- */

#ifdef ZENITH_TEST_PLUGIN_DIR

ZTEST(PluginManager, LoadStartQueryUnload) {
    plugin::ServiceRegistry reg;
    concurrency::ThreadPool pool(2);
    core::KeyValueConfig config;
    config.set("hello_c.greeting_prefix", "Bonjour");

    plugin::PluginManager mgr(reg, &pool, &config);
    auto loaded = mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);
    if (!loaded) {
        ztest::reportFail(__FILE__, __LINE__,
                          "loadDirectory failed: " + loaded.error().message);
    }
    ZEXPECT_TRUE(loaded.value() >= 1);

    // 两个示例插件都被发现（hello_cpp + hello_c）
    bool hasCpp = false, hasC = false;
    for (const auto& p : mgr.plugins()) {
        if (p->name == std::string("hello_cpp")) hasCpp = true;
        if (p->name == std::string("hello_c")) hasC = true;
    }
    ZEXPECT_TRUE(hasCpp);
    ZEXPECT_TRUE(hasC);

    ZEXPECT_TRUE(static_cast<bool>(mgr.startAll()));

    // C++ 服务
    zenith_status st = ZENITH_OK;
    void* raw = reg.queryRaw(example::IGreeter::service_name(), 1, &st);
    ZEXPECT_EQ(st, ZENITH_OK);
    if (raw != nullptr) {
        ZEXPECT_EQ(static_cast<example::IGreeter*>(raw)->greet("Zenith"),
                   std::string("Hello, Zenith! (from hello_cpp plugin)"));
    }

    // 纯 C 服务
    void* craw = reg.queryRaw(ZENITH_C_GREETER_NAME, ZENITH_C_GREETER_VERSION, &st);
    ZEXPECT_EQ(st, ZENITH_OK);
    if (craw != nullptr) {
        char buf[256];
        auto* cg = static_cast<zenith_c_greeter*>(craw);
        const char* s = cg->vt->greet(craw, "Zenith", buf, sizeof(buf));
        ZEXPECT_TRUE(s != nullptr);
        if (s != nullptr) {
            ZEXPECT_EQ(std::string(s), std::string("Bonjour, Zenith! (from hello_c plugin, pure C)"));
        }
    }

    // 全量卸载：服务清理干净、不崩溃
    mgr.unloadAll();
    ZEXPECT_EQ(reg.size(), size_t{0});
}

ZTEST(PluginManager, LoadNonPluginFails) {
    plugin::ServiceRegistry reg;
    plugin::PluginManager mgr(reg, nullptr, nullptr);
    auto r = mgr.load("does_not_exist.dll");
    ZEXPECT_TRUE(r.isErr());
}

#else

ZTEST(PluginManager, SkippedNoPluginDir) {
    ZEXPECT_TRUE(true); // ZENITH_BUILD_PLUGINS=OFF 时占位
}

#endif

ZTEST_MAIN
