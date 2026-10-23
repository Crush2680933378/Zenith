// ============================================================================
//  ai_crew —— C ABI 插件：多 Agent 软件开发协作引擎（M11 / SDLC-Crew）
//  单插件承载 7 角色（PM/架构师/后端/前端/测试/运维/交付）——共享编排状态机/
//  产物库/事件总线，内聚不可拆。推理经 IInferenceService（OpenAI API 直通或本地引擎）。
//  HTTP 挂载由宿主经 crew_api.hpp 的 mountCrewApi 完成（REST + SSE 状态板）。
// ============================================================================
#include <new>

#include "zenith/abi/zenith_abi.h"

#include "zenith/crew/crew.hpp"
#include "zenith/plugin/service_registry.hpp"

namespace {

const zenith_host_api* g_host = nullptr;
zenith::crew::CrewEngine* g_engine = nullptr;

void destroyEngine(void* p) {
    delete static_cast<zenith::crew::CrewEngine*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_crew", 0,
                    "ai_crew: init (7-role SDLC crew: PM/architect/backend/frontend/"
                    "tester/devops/delivery; voting + artifacts + event bus)");

    auto* engine = new (std::nothrow) zenith::crew::CrewEngine();
    if (engine == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    g_engine = engine;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::crew::ICrewService::service_name(),
                                  zenith::crew::ICrewService::service_version(),
                                  static_cast<void*>(engine), &destroyEngine);
}

zenith_status pluginStart(void) {
    if (g_host == nullptr || g_engine == nullptr) return ZENITH_ERR_GENERIC;
    auto& reg = *static_cast<zenith::plugin::ServiceRegistry*>(g_host->registry_ctx);
    if (auto inf = reg.get<zenith::ai::IInferenceService>()) {
        g_engine->setInference(inf);
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "ai_crew", 0,
                          "ai_crew: bound inference backend");
    } else {
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_WARN, "ai_crew", 0,
                          "ai_crew: no IInferenceService found — load ai_inference first");
    }
    return ZENITH_OK;
}

zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    g_engine = nullptr; // 所有权归宿主（destroyEngine 删除）
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_crew",
    "0.1.0",
    "Zenith multi-agent SDLC crew: 7 roles, voting decisions, artifact store, live SSE",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
