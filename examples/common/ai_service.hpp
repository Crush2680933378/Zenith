// ============================================================================
//  IAiDrawService —— AI 绘画共享接口（宿主与 ai-comfyui 插件之间）
//  对接 ComfyUI HTTP API：
//    POST /prompt          提交 workflow（api 格式 JSON，占位符已注入）
//    GET  /history/{id}    轮询执行状态/产物
//    GET  /view            下载产物图片
// ============================================================================
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "zenith/plugin/service.hpp"

namespace zenith::ai {

struct DrawResult {
    bool ok = false;
    std::string promptId;
    std::vector<std::string> outputs;   // 已保存的产物文件路径
    std::string error;
};

/// workflow 占位符注入：{{seed}} 随机、{{text:<key>}} 文本替换
using PlaceholderResolver = std::function<std::string(const std::string& key)>;

class IAiDrawService : public zenith::plugin::IService {
public:
    ZENITH_SERVICE_DECL(IAiDrawService, "zenith.ai.IAiDrawService", 1)

    /// 提交并等待完成（阻塞调用方线程——经线程池使用为宜）
    virtual DrawResult draw(const std::string& workflowJson,
                            const std::string& outputDir,
                            PlaceholderResolver resolver = nullptr,
                            int timeoutSec = 300) = 0;

    /// 异步形态：提交即返回 prompt_id，进度经轮询线程触发
    using ProgressCallback = std::function<void(const std::string& promptId, double progress)>;
    virtual std::string submit(const std::string& workflowJson,
                               const std::string& outputDir,
                               PlaceholderResolver resolver,
                               std::function<void(DrawResult)> onDone,
                               ProgressCallback onProgress = nullptr) = 0;
};

} // namespace zenith::ai
