// ============================================================================
//  zenith::crew —— 多 Agent 协作软件开发引擎（M11 / SDLC-Crew）
//
//  设计哲学：确定性状态机编排 + LLM 只做内容生成 + 门禁可执行校验。
//  用户视角：POST /crew/tasks 一句话开任务，无需感知内部 7 个 Agent。
//
//  角色（7）：PM / 架构师 / 后端 / 前端 / 测试 / 运维 / 交付
//  协作面：共享产物库（版本化）+ 投票决策（多数决，架构师加权破平）+ 事件总线
//  实时观测：事件流（轮询 /crew/tasks/:id/events + SSE 流式），
//            状态板 /crew/tasks/:id/board —— 前端仪表盘就绪的稳定 JSON 模式。
//
//  推理后端：IInferenceService（即 ai_inference 插件——OpenAI API 直通或本地引擎）。
// ============================================================================
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zenith/ai/inference_service.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"
#include "zenith/plugin/service.hpp"

namespace zenith::crew {

enum class CrewPhase : uint8_t {
    Pending = 0,
    Requirements,   // PM 产 PRD
    ReviewPrd,      // 全员投票：PRD 是否过关
    Design,         // 架构师产契约（含技术选型投票）
    Implement,      // 后端 ∥ 前端（对契约并行）
    Test,           // 测试产用例并执行
    Bugfix,         // 缺陷路由修复环（上限 maxBugCycles）
    Deploy,         // 运维产 CI/CD + 部署
    Release,        // 交付产 CHANGELOG + 发布
    AwaitingApproval, // 人工门禁等待
    Done,
    Failed,
};

ZENITH_API const char* phaseName(CrewPhase p) noexcept;

struct CrewEvent {
    uint64_t seq = 0;
    int64_t tsMs = 0;
    std::string taskId;
    CrewPhase phase = CrewPhase::Pending;
    std::string agent;    // 角色名（编排事件为 "orchestrator"）
    std::string type;     // phase|agent_start|agent_done|artifact|vote|gate|approval|error
    std::string detail;
    std::string dataJson; // 附加载荷（投票明细/产物摘要等）

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct Artifact {
    std::string id;       // <type>-v<version>
    std::string taskId;
    std::string type;     // prd/architecture/contract/backend/frontend/tests/bugs/devops/release
    std::string agent;
    int version = 1;
    std::string content;  // 文本（代码/文档/JSON）
    int64_t tsMs = 0;

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct VoteRecord {
    uint64_t id = 0;
    std::string taskId;
    std::string question;
    std::vector<std::string> options;
    std::vector<std::pair<std::string, std::string>> votes; // (agent, choice)
    std::string winner;
    std::string reason;    // 中选理由（取胜出票的理由）
    int64_t tsMs = 0;

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

/// 前端状态板单元：一个角色的实时快照
struct AgentStatus {
    std::string role;
    std::string state;    // idle|working|voting|waiting|done|failed
    std::string detail;
    int64_t updatedMs = 0;

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct CrewTaskInfo {
    std::string id;
    std::string brief;
    CrewPhase phase = CrewPhase::Pending;
    bool running = false;
    std::string error;
    std::vector<AgentStatus> agents;   // 7 角色快照
    size_t eventCount = 0;
    size_t artifactCount = 0;
    size_t voteCount = 0;
    int64_t createdMs = 0;
    int64_t updatedMs = 0;

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct CrewConfig {
    std::string model;                  // 空 = 推理服务默认
    int maxBugCycles = 2;
    bool requireApproval = false;       // 关键门禁转人工
    int voteTimeoutSec = 0;             // 预留
};

class ICrewService : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(ICrewService, "zenith.crew.ICrewService", 1)

    /// 开一个 Agent 集群任务（异步执行；返回 taskId）
    [[nodiscard]] virtual std::string createTask(const std::string& brief,
                                                 const CrewConfig& cfg) = 0;
    std::string createTask(const std::string& brief) {
        return createTask(brief, CrewConfig{});
    }
    [[nodiscard]] virtual core::Result<CrewTaskInfo> taskInfo(const std::string& id) = 0;
    [[nodiscard]] virtual std::vector<CrewTaskInfo> listTasks() = 0;
    [[nodiscard]] virtual std::vector<CrewEvent> events(const std::string& id,
                                                        uint64_t sinceSeq = 0) = 0;
    [[nodiscard]] virtual std::vector<Artifact> artifacts(const std::string& id) = 0;
    [[nodiscard]] virtual std::vector<VoteRecord> votes(const std::string& id) = 0;
    /// 人工放行（AWAITING_APPROVAL → 继续）
    [[nodiscard]] virtual core::Result<void> approve(const std::string& id) = 0;

    /// SSE 实时订阅：回调在编排线程触发（轻量；重活自行投递）
    using EventCallback = std::function<void(const CrewEvent&)>;
    [[nodiscard]] virtual uint64_t subscribe(const std::string& taskId, EventCallback cb) = 0;
    virtual void unsubscribe(uint64_t subId) = 0;

    virtual void setInference(std::shared_ptr<ai::IInferenceService> inf) = 0;
};

// ---------------------------------------------------------------------------
// CrewEngine —— 具体引擎（宿主/测试可直接实例化，不经插件）
// ---------------------------------------------------------------------------
class ZENITH_API CrewEngine : public ICrewService {
public:
    CrewEngine();
    ~CrewEngine() override;

    void setInference(std::shared_ptr<ai::IInferenceService> inf) override;

    using ICrewService::createTask;
    std::string createTask(const std::string& brief, const CrewConfig& cfg) override;
    /// 单参便捷（虚接口不能带默认参）
    std::string createTask(const std::string& brief) { return createTask(brief, CrewConfig{}); }
    core::Result<CrewTaskInfo> taskInfo(const std::string& id) override;
    std::vector<CrewTaskInfo> listTasks() override;
    std::vector<CrewEvent> events(const std::string& id, uint64_t sinceSeq) override;
    std::vector<CrewEvent> events(const std::string& id) { return events(id, 0); }
    std::vector<Artifact> artifacts(const std::string& id) override;
    std::vector<VoteRecord> votes(const std::string& id) override;
    core::Result<void> approve(const std::string& id) override;
    uint64_t subscribe(const std::string& taskId, EventCallback cb) override;
    void unsubscribe(uint64_t subId) override;

    // ---- 测试/复用入口：单次投票（多数决；架构师票权 2）----
    struct VoteSpec {
        std::string question;
        std::vector<std::string> options;
        std::vector<std::string> voters; // 角色名；空 = 全员
    };
    [[nodiscard]] VoteRecord runVote(const std::string& taskId, const VoteSpec& spec);

    /// 内部状态注入（测试用剧本推理）
    void setBusy(const std::string& taskId, const std::string& role,
                 const std::string& state, const std::string& detail);

private:
    struct TaskState {
        std::string id;
        std::string brief;
        CrewConfig cfg;
        CrewPhase phase = CrewPhase::Pending;
        bool running = false;
        std::string error;
        std::vector<CrewEvent> events;
        std::vector<Artifact> artifacts;
        std::vector<VoteRecord> votes;
        std::map<std::string, AgentStatus> agents; // role → status
        int64_t createdMs = 0;
        int64_t updatedMs = 0;
        bool approved = false; // 人工放行标志
        // 测试钩子：每角色剧本计数（见 crew.cpp 推理分发）
        std::map<std::string, int> roleCalls;
    };
    using TaskPtr = std::shared_ptr<TaskState>;

    void runPipeline(TaskPtr t);                       // 编排主循环（独立线程）
    void emit(TaskPtr t, CrewPhase phase, const std::string& agent,
              const std::string& type, const std::string& detail,
              const std::string& dataJson = {});
    [[nodiscard]] std::string chat(TaskPtr t, const std::string& role,
                                   const std::string& userPrompt);
    void saveArtifact(TaskPtr t, const std::string& type, const std::string& agent,
                      const std::string& content);
    [[nodiscard]] static json::JsonValue extractJson(const std::string& text);

    std::shared_ptr<ai::IInferenceService> inference_;
    std::mutex mtx_;                        // tasks_/subs_ 保护
    std::map<std::string, TaskPtr> tasks_;
    uint64_t subSeq_ = 1;
    std::map<uint64_t, std::pair<std::string, EventCallback>> subs_;
    std::atomic<uint64_t> taskSeq_{0};
};

} // namespace zenith::crew
