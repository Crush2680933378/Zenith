// ============================================================================
//  ai_router —— C ABI 插件：多厂商 LLM API 路由（M12）
//  配置来源：宿主 config_get("ai.router.config")（JSON 字符串，格式见
//  provider_router.hpp 文件头）。装载成功后注册 IInferenceService——
//  ai_crew/ai_agent/openai_api 网关零改动获得多厂商能力。
// ============================================================================
#include <new>

#include "zenith/abi/zenith_abi.h"

#include "zenith/ai/provider_router.hpp"
#include "zenith/json/json.hpp"

namespace {

const zenith_host_api* g_host = nullptr;
zenith::ai::ProviderRouter* g_router = nullptr;

void destroyRouter(void* p) {
    delete static_cast<zenith::ai::ProviderRouter*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    // 配置：宿主注入（ai.router.config = JSON 字符串；$ENV: 可引用密钥）
    std::string cfgJson;
    const char* v = nullptr;
    if (host->config_get(host->config_ctx, "ai.router.config", "", &v) == ZENITH_OK &&
        v != nullptr) {
        cfgJson = v;
    }
    if (cfgJson.empty()) {
        host->log_write(host->log_ctx, ZENITH_LOG_WARN, "ai_router", 0,
                        "ai_router: no ai.router.config set — plugin idle");
        return ZENITH_OK; // 空配置合法装载（不注册服务）
    }

    auto parsed = zenith::json::JsonValue::parse(cfgJson);
    if (!parsed) {
        host->log_write(host->log_ctx, ZENITH_LOG_ERROR, "ai_router", 0,
                        ("ai_router: bad config JSON: " + parsed.error().message).c_str());
        return ZENITH_ERR_PLUGIN_INIT_FAILED;
    }
    auto cfg = zenith::ai::RouterConfig::fromJson(parsed.value());
    if (!cfg) {
        host->log_write(host->log_ctx, ZENITH_LOG_ERROR, "ai_router", 0,
                        ("ai_router: " + cfg.error().message).c_str());
        return ZENITH_ERR_PLUGIN_INIT_FAILED;
    }

    auto* router = new (std::nothrow) zenith::ai::ProviderRouter(std::move(cfg).value());
    if (router == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    g_router = router;

    std::string names;
    for (const auto& p : router->config().providers) {
        names += p.name + "(" + zenith::ai::providerApiName(p.api) + ") ";
    }
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_router", 0,
                    ("ai_router: multi-provider ready — " + names).c_str());
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::ai::IInferenceService::service_name(),
                                  zenith::ai::IInferenceService::service_version(),
                                  static_cast<void*>(router), &destroyRouter);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    g_router = nullptr; // 宿主 destroyRouter 负责删除
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_router",
    "0.1.0",
    "Zenith multi-provider LLM router: openai-chat/anthropic/gemini adapters (DeepSeek "
    "Harness style provider config)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
