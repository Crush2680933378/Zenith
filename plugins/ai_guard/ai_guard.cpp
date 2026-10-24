// ============================================================================
//  ai_guard —— C ABI 插件：LLM 输入安全护栏（M13 第二批）
//  注册 IGuardService（GuardServiceImpl 内核即核心库 guard_service）。
//  配置来源：宿主 config_get("ai.guard.config")：
//    {"blockScore":60, "extraPhrases":["内部机密词"], "allowlist":["样例邮箱@x"]}
//  宿主 HTTP 挂载：#include "zenith/ai/guard_service.hpp" 后
//    mountGuardApi(server.router(), inspector)
// ============================================================================
#include <new>

#include "zenith/abi/zenith_abi.h"

#include "zenith/ai/guard_service.hpp"
#include "zenith/json/json.hpp"

namespace {

const zenith_host_api* g_host = nullptr;
zenith::ai::GuardServiceImpl* g_guard = nullptr;

void destroyGuard(void* p) {
    delete static_cast<zenith::ai::GuardServiceImpl*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    zenith::ai::GuardInspector::Config cfg;
    std::string cfgJson;
    const char* v = nullptr;
    if (host->config_get(host->config_ctx, "ai.guard.config", "", &v) == ZENITH_OK &&
        v != nullptr) {
        cfgJson = v;
    }
    if (!cfgJson.empty()) {
        auto parsed = zenith::json::JsonValue::parse(cfgJson);
        if (!parsed) {
            host->log_write(host->log_ctx, ZENITH_LOG_ERROR, "ai_guard", 0,
                            ("ai_guard: bad config JSON: " + parsed.error().message).c_str());
            return ZENITH_ERR_PLUGIN_INIT_FAILED;
        }
        const zenith::json::JsonValue& j = parsed.value();
        cfg.blockScore = static_cast<int>(j["blockScore"].asInt(60));
        if (const auto* ex = j["extraPhrases"].asArray()) {
            for (const auto& e : *ex) cfg.extraPhrases.push_back(e.asString());
        }
        if (const auto* al = j["allowlist"].asArray()) {
            for (const auto& a : *al) cfg.allowlist.push_back(a.asString());
        }
    }

    auto* guard = new (std::nothrow)
        zenith::ai::GuardServiceImpl(zenith::ai::GuardInspector(std::move(cfg)));
    if (guard == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    g_guard = guard;

    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_guard", 0,
                    "ai_guard: ready (prompt-injection + PII: "
                    "email/phone/cn-id/card/apikey)");
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::ai::IGuardService::service_name(),
                                  zenith::ai::IGuardService::service_version(),
                                  static_cast<void*>(guard), &destroyGuard);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    g_guard = nullptr; // 所有权归宿主（destroyGuard 删除）
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_guard",
    "0.1.0",
    "Zenith LLM input guard: prompt-injection phrases + PII scan/redact (local rules)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
