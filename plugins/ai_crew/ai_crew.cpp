// ============================================================================
//  ai_crew —— C ABI 插件：多 Agent 软件开发协作引擎（M11 / SDLC-Crew，M14 强化）
//  单插件承载 7 角色（可配置覆盖/扩展）——共享编排状态机/产物库/事件总线。
//  推理经 IInferenceService（ai_llama / ai_router / ai_inference 任一）。
//  HTTP 挂载由宿主经 crew_api.hpp 的 mountCrewApi 完成（REST + SSE + 仪表盘）。
//
//  引擎级配置（宿主 config_get）：
//    crew.persist.dir        任务/组织记忆落盘目录（空 = 纯内存）
//    crew.max.concurrent     并发任务上限（默认 4）
//    crew.approval.token     写操作令牌（非空 = approve/cancel/resume/vote 需
//                            X-Crew-Token 头；mountCrewApi 第三参传入同值）
//    crew.webhook.url        终态/待审批回调
//    crew.roles.json         角色覆盖 [{role,prompt,voteWeight}]
//    crew.memory.enabled     组织记忆开关（默认 true）
//    crew.cost.promptPerM    成本估算 $/百万 prompt token
//    crew.cost.completionPerM 成本估算 $/百万 completion token
//    crew.max.prompt.chars    提示词字符上限（默认 24000；-1 关闭截断）
// ============================================================================
#include <cstdlib>
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

std::string confString(const zenith_host_api* host, const char* key) {
    const char* v = nullptr;
    if (host->config_get(host->config_ctx, key, "", &v) == ZENITH_OK && v != nullptr) {
        return v;
    }
    return {};
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;

    zenith::crew::EngineOptions opt;
    opt.persistDir = confString(host, "crew.persist.dir");
    opt.maxConcurrentTasks =
        std::max(1, std::atoi(confString(host, "crew.max.concurrent").c_str()));
    opt.approvalToken = confString(host, "crew.approval.token");
    opt.webhookUrl = confString(host, "crew.webhook.url");
    opt.rolesJson = confString(host, "crew.roles.json");
    opt.memoryEnabled = confString(host, "crew.memory.enabled") != "false";
    if (!opt.persistDir.empty() && opt.maxConcurrentTasks == 1 &&
        confString(host, "crew.max.concurrent").empty()) {
        opt.maxConcurrentTasks = 4; // atoi("")==0 兜底
    }
    {
        const std::string pp = confString(host, "crew.cost.promptPerM");
        const std::string pc = confString(host, "crew.cost.completionPerM");
        if (!pp.empty()) opt.pricePromptPerM = std::atof(pp.c_str());
        if (!pc.empty()) opt.priceCompletionPerM = std::atof(pc.c_str());
        const std::string mp = confString(host, "crew.max.prompt.chars");
        if (!mp.empty()) opt.maxPromptChars = std::atoi(mp.c_str());
    }

    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_crew", 0,
                    ("ai_crew: init (SDLC crew; persist=" +
                     (opt.persistDir.empty() ? std::string("off") : opt.persistDir) +
                     " maxConcurrent=" + std::to_string(opt.maxConcurrentTasks) + ")")
                        .c_str());

    auto* engine = new (std::nothrow) zenith::crew::CrewEngine(std::move(opt));
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
                          "ai_crew: no IInferenceService found — load inference first");
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
    "0.2.0",
    "Zenith multi-agent SDLC crew: roles, debates, artifacts, persistence, memory, cost",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
