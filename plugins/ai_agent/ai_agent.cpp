// ============================================================================
//  ai_agent —— C ABI 插件：Agent 运行时服务（M10.3）
//  组装 IInferenceService（必需）+ IVectorStoreService（可选长期记忆）→
//  Agent 实例池（按需创建）；以 IAgentService 暴露。
//  宿主侧 HTTP 挂载见 08_ai_agent 示例（/agent/run 端点）。
// ============================================================================
#include <map>
#include <memory>
#include <new>
#include <mutex>

#include "zenith/abi/zenith_abi.h"

#include "zenith/agent/agent.hpp"
#include "zenith/vector/vector_service.hpp"
#include "zenith/plugin/service_registry.hpp"

namespace {

const zenith_host_api* g_host = nullptr;

class AgentService final : public zenith::plugin::IService {
public:
    static constexpr const char* kName = "zenith.agent.IAgentService";
    static constexpr uint32_t kVersion = 1;

    [[nodiscard]] const char* serviceInterfaceName() const noexcept override { return kName; }
    [[nodiscard]] uint32_t serviceInterfaceVersion() const noexcept override { return kVersion; }

    void setInference(std::shared_ptr<zenith::ai::IInferenceService> inf) {
        std::lock_guard<std::mutex> lk(mtx_);
        inference_ = std::move(inf);
        agents_.clear(); // 引擎换实例 → Agent 重建
    }
    void setVector(void* vecRaw) {
        std::lock_guard<std::mutex> lk(mtx_);
        vectorRaw_ = vecRaw;
    }

    /// 取（或懒建）Agent。cfg 定制后传入 custom=true 隔离命名实例。
    zenith::agent::Agent& agent(const std::string& key = "default") {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = agents_.find(key);
        if (it == agents_.end()) {
            zenith::agent::AgentConfig cfg;
            auto a = std::make_unique<zenith::agent::Agent>(inference_, cfg);
            if (vectorRaw_ != nullptr) {
                a->remember(vectorRaw_, "", 0); // 注入向量服务（记忆钩子由宿主配）
            }
            it = agents_.emplace(key, std::move(a)).first;
        }
        return *it->second;
    }

    [[nodiscard]] bool ready() const {
        std::lock_guard<std::mutex> lk(mtx_);
        return inference_ != nullptr;
    }

private:
    mutable std::mutex mtx_;
    std::shared_ptr<zenith::ai::IInferenceService> inference_;
    void* vectorRaw_ = nullptr;
    std::map<std::string, std::unique_ptr<zenith::agent::Agent>> agents_;
};

AgentService* g_svc = nullptr;

void destroyAgentService(void* p) {
    delete static_cast<AgentService*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_agent", 0,
                    "ai_agent: init (ReAct tool-loop over IInferenceService)");

    auto* svc = new (std::nothrow) AgentService();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    g_svc = svc;  // 所有权归宿主（destroy 回调删除）
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  AgentService::kName, AgentService::kVersion,
                                  static_cast<void*>(svc), &destroyAgentService);
}

zenith_status pluginStart(void) {
    if (g_host == nullptr) return ZENITH_ERR_GENERIC;
    auto& reg = *static_cast<zenith::plugin::ServiceRegistry*>(g_host->registry_ctx);
    if (auto inf = reg.get<zenith::ai::IInferenceService>()) {
        g_svc->setInference(inf);
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "ai_agent", 0,
                          "ai_agent: bound inference backend");
    } else {
        g_host->log_write(g_host->log_ctx, ZENITH_LOG_WARN, "ai_agent", 0,
                          "ai_agent: inference service not found (load ai_inference first)");
    }
    if (auto vec = reg.get<zenith::vector::IVectorStoreService>()) {
        g_svc->setVector(static_cast<void*>(vec.get()));
    }
    return ZENITH_OK;
}

zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    g_svc = nullptr;  // 宿主 destroyAgentService 负责删除
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_agent",
    "0.1.0",
    "Zenith agent runtime: ReAct tool-loop + session memory + RAG recall",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
