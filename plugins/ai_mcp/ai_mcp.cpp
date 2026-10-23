// ============================================================================
//  ai_mcp —— C ABI 插件：MCP 网关（M10.2）
//  init 时创建 McpServer 并桥接注册表里可用的服务（向量库/推理），
//  以 IMcpGatewayService 暴露给宿主；宿主经 mcp_api.hpp 挂 HTTP 端点。
//  远端工具接入：宿主把 McpClient 的工具注册进本服务即可让 Agent 调用
//  外部 MCP 服务器（见 ai_agent 的 remote tools 集成）。
// ============================================================================
#include <memory>
#include <new>
#include <string>

#include "zenith/abi/zenith_abi.h"

#include "zenith/ai/inference_service.hpp"
#include "zenith/mcp/mcp.hpp"
#include "zenith/vector/vector_service.hpp"
#include "zenith/plugin/service_registry.hpp"

namespace {

const zenith_host_api* g_host = nullptr;

/// 服务接口：宿主取 McpServer 实例挂 HTTP / 注册额外工具
class McpGatewayService final : public zenith::plugin::IService {
public:
    static constexpr const char* kName = "zenith.mcp.IMcpGatewayService";
    static constexpr uint32_t kVersion = 1;

    [[nodiscard]] const char* serviceInterfaceName() const noexcept override { return kName; }
    [[nodiscard]] uint32_t serviceInterfaceVersion() const noexcept override { return kVersion; }

    std::shared_ptr<zenith::mcp::McpServer> server;
};

McpGatewayService* g_svc = nullptr;

void destroyGateway(void* p) {
    delete static_cast<McpGatewayService*>(p);
}

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_mcp", 0,
                    "ai_mcp: init (JSON-RPC 2.0 over HTTP, protocol 2026-07-28)");

    auto* svc = new (std::nothrow) McpGatewayService();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    svc->server = std::make_shared<zenith::mcp::McpServer>("zenith-mcp", "0.1.0");

    // 桥接注册表里可用的服务（缺什么跳什么——插件加载顺序解耦）
    auto& reg = *static_cast<zenith::plugin::ServiceRegistry*>(host->registry_ctx);
    if (auto vec = reg.get<zenith::vector::IVectorStoreService>()) {
        svc->server->bridgeVector(static_cast<void*>(vec.get()));
        host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_mcp", 0,
                        "ai_mcp: bridged vector_search/vector_upsert");
    }
    if (auto inf = reg.get<zenith::ai::IInferenceService>()) {
        svc->server->bridgeInference(static_cast<void*>(inf.get()));
        host->log_write(host->log_ctx, ZENITH_LOG_INFO, "ai_mcp", 0,
                        "ai_mcp: bridged chat");
    }

    g_svc = svc;  // 所有权归宿主（destroy 回调删除）——此处仅持引用
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  McpGatewayService::kName, McpGatewayService::kVersion,
                                  static_cast<void*>(svc), &destroyGateway);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) {
    // 服务实例由宿主 destroyGateway 删除（唯一所有权）；这里只清引用
    g_svc = nullptr;
    g_host = nullptr;
}

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "ai_mcp",
    "0.1.0",
    "Zenith MCP gateway: expose DB/vector/inference as AI-agent tools (JSON-RPC)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
