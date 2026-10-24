// ============================================================================
//  zenith::crew —— 多 Agent 协作软件开发引擎（M11 / SDLC-Crew，M14 全面强化）
//
//  设计哲学：确定性状态机编排 + LLM 只做内容生成 + 门禁可执行校验。
//  用户视角：POST /crew/tasks 一句话开任务，无需感知内部 Agent。
//
//  角色（默认 7，可配置覆盖/扩展）：PM/架构师/后端/前端/测试/运维/交付
//  协作面：版本化产物库 + 加权投票（多轮辩论可选）+ 事件总线（含 token 流）
//  M14 强化：
//    · 每角色独立模型路由 + 结构化输出自修正重试 + 推理调用重试
//    · 取消/暂停恢复、任务持久化与断点续跑、受管线程池（并发上限）
//    · 真实缺陷环（结构化 issue 派单）、后端∥前端真并行、story 子环并行
//    · 人工角色（投票/审批评论）、组织记忆（跨任务经验检索注入）
//    · token/成本记账（任务级 + 角色级）、OTel span 全链路、质量评分卡
//    · 产物打包导出、webhook 回调、可选真执行验证（exec verify）
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zenith/ai/inference_service.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/json/json.hpp"
#include "zenith/plugin/service.hpp"

namespace zenith::obs {
class OtelExporter;
} // namespace zenith::obs

namespace zenith::crew {

enum class CrewPhase : uint8_t {
    Pending = 0,
    Requirements,   // PM 产 PRD
    ReviewPrd,      // 全员投票：PRD 是否过关（可多轮辩论）
    Design,         // 架构师产契约（含技术选型投票）
    Implement,      // 后端 ∥ 前端（真并行；story 模式下按 story 子环并行）
    Test,           // 测试产用例并执行（可选真执行验证）
    Bugfix,         // 结构化 issue 派单修复环（上限 maxBugCycles）
    Deploy,         // 运维产 CI/CD + 部署
    Release,        // 交付产 CHANGELOG + 发布 + 质量评分卡
    AwaitingApproval, // 人工门禁等待
    Done,
    Failed,
    Cancelled,      // M14：用户主动取消
};

ZENITH_API const char* phaseName(CrewPhase p) noexcept;

struct CrewEvent {
    uint64_t seq = 0;
    int64_t tsMs = 0;
    std::string taskId;
    CrewPhase phase = CrewPhase::Pending;
    std::string agent;    // 角色名（编排事件为 "orchestrator"）
    std::string type;     // phase|agent_start|agent_done|artifact|vote|gate|approval|
                          // error|token|retry|cancelled|human_vote_required|
                          // exec|memory|cost|webhook
    std::string detail;
    std::string dataJson; // 附加载荷（投票明细/产物摘要/token 增量等）

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct Artifact {
    std::string id;       // <type>-v<version>
    std::string taskId;
    std::string type;     // prd/architecture/contract/backend/frontend/tests/bugs/
                          // devops/release/scorecard/story-<id>-*/memory
    std::string agent;
    int version = 1;
    std::string content;
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
    std::string reason;
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

/// 单角色用量/成本记账（M14）
struct RoleUsage {
    uint64_t calls = 0;
    uint64_t tokensIn = 0;
    uint64_t tokensOut = 0;
    double costUsd = 0.0;
};

struct CrewTaskInfo {
    std::string id;
    std::string brief;
    CrewPhase phase = CrewPhase::Pending;
    bool running = false;
    bool resumable = false;             // M14：中断后可 resume
    std::string error;
    std::vector<AgentStatus> agents;
    size_t eventCount = 0;
    size_t artifactCount = 0;
    size_t voteCount = 0;
    int64_t createdMs = 0;
    int64_t updatedMs = 0;
    double costUsd = 0.0;               // M14：任务级成本
    uint64_t tokensIn = 0;
    uint64_t tokensOut = 0;
    std::map<std::string, RoleUsage> roleUsage; // M14：角色级明细

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
};

struct CrewConfig {
    std::string model;                  // 默认模型（空 = 推理服务默认）
    std::map<std::string, std::string> roleModels; // 每角色模型覆盖（M14）
    int maxBugCycles = 2;
    bool requireApproval = false;
    int approvalTimeoutSec = 3600;
    // ---- M14 扩展（默认值保持 M11 行为不变；见各字段注释）----
    bool parallelImplement = true;      // 后端∥前端真并行
    bool strictRetest = true;           // 重测提示要求基于修复验证（含 RETEST 关键字）
    int jsonRetryLimit = 2;             // 结构化输出自修正重试次数（0 = 关）
    bool streamTokens = true;           // 推理 token 增量入事件流
    bool execVerify = false;            // 测试命令真执行（opt-in；结果以退出码为准）
    bool useMemory = true;              // 组织记忆：开工检索/完工归档
    bool storiesMode = false;           // PRD story 子环并行实现
    int prdVoteRounds = 1;              // PRD 评审轮数（>1 = revise 后再投票）
    bool humanVoter = false;            // 人工作为额外投票人（等待 HTTP 投票）
    int humanVoteTimeoutSec = 600;

    [[nodiscard]] ZENITH_API json::JsonValue toJson() const;
    ZENITH_API static CrewConfig fromJson(const json::JsonValue& v);
};

/// 引擎级选项（宿主/插件注入；setOptions 在 createTask 前调用）
struct EngineOptions {
    std::string persistDir;             // 任务/记忆落盘目录（空 = 纯内存）
    int maxConcurrentTasks = 4;         // 受管线程池上限
    std::string approvalToken;          // 非空 = 写操作需 X-Crew-Token 匹配
    std::string webhookUrl;             // 终态/待审批回调（POST JSON）
    std::string rolesJson;              // 角色覆盖 JSON：[{role,prompt,voteWeight}]
    bool memoryEnabled = true;          // 组织记忆总开关
    double pricePromptPerM = 0.0;       // 成本估算（$/百万 token；0 = 只记 token）
    double priceCompletionPerM = 0.0;
    std::shared_ptr<obs::OtelExporter> otel; // 全链路 span 导出
};

class ICrewService : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(ICrewService, "zenith.crew.ICrewService", 2)

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
    /// 带评论放行（默认实现回落无评论版——旧实现者零改动）
    [[nodiscard]] virtual core::Result<void> approve(const std::string& id,
                                                     const std::string& /*comment*/) {
        return approve(id);
    }
    /// M14：取消运行中任务（协作式：phase/chat 边界生效）
    [[nodiscard]] virtual core::Result<void> cancel(const std::string&) {
        return core::Result<void>::Err(core::Error(304, "cancel not supported"));
    }
    /// M14：断点恢复（仅重启/中断后 resumable 的任务）
    [[nodiscard]] virtual core::Result<void> resume(const std::string&) {
        return core::Result<void>::Err(core::Error(304, "resume not supported"));
    }
    /// M14：人工投票（humanVoter 等待中的票）
    [[nodiscard]] virtual core::Result<void> castHumanVote(const std::string&,
                                                           const std::string&) {
        return core::Result<void>::Err(core::Error(304, "human vote not supported"));
    }
    /// M14：任务全量导出 JSON 包（task+artifacts+votes+events；空串=不支持）
    [[nodiscard]] virtual std::string exportBundle(const std::string&) { return {}; }

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
    explicit CrewEngine(EngineOptions opt);
    ~CrewEngine() override; // 等待受管线程收尾（运行中任务被标记中断）

    void setInference(std::shared_ptr<ai::IInferenceService> inf) override;
    void setOptions(EngineOptions opt); // createTask 前调用

    using ICrewService::createTask;
    std::string createTask(const std::string& brief, const CrewConfig& cfg) override;
    std::string createTask(const std::string& brief) { return createTask(brief, CrewConfig{}); }
    core::Result<CrewTaskInfo> taskInfo(const std::string& id) override;
    std::vector<CrewTaskInfo> listTasks() override;
    std::vector<CrewEvent> events(const std::string& id, uint64_t sinceSeq) override;
    std::vector<CrewEvent> events(const std::string& id) { return events(id, 0); }
    std::vector<Artifact> artifacts(const std::string& id) override;
    std::vector<VoteRecord> votes(const std::string& id) override;
    core::Result<void> approve(const std::string& id) override;
    core::Result<void> approve(const std::string& id, const std::string& comment) override;
    core::Result<void> cancel(const std::string& id) override;
    core::Result<void> resume(const std::string& id) override;
    core::Result<void> castHumanVote(const std::string& id, const std::string& choice) override;
    std::string exportBundle(const std::string& id) override;
    uint64_t subscribe(const std::string& taskId, EventCallback cb) override;
    void unsubscribe(uint64_t subId) override;

    // ---- 测试/复用入口：单次投票（加权多数；可含 human 选民）----
    struct VoteSpec {
        std::string question;
        std::vector<std::string> options;
        std::vector<std::string> voters; // 角色名/“human”；空 = 全员
    };
    [[nodiscard]] VoteRecord runVote(const std::string& taskId, const VoteSpec& spec);

    /// 内部状态注入（测试用剧本推理）
    void setBusy(const std::string& taskId, const std::string& role,
                 const std::string& state, const std::string& detail);

    [[nodiscard]] const EngineOptions& options() const noexcept { return opt_; }

private:
    struct TaskState {
        std::string id;
        std::string brief;
        CrewConfig cfg;
        CrewPhase phase = CrewPhase::Pending;
        bool running = false;
        bool resumable = false;
        bool resumeMode = false;   // resume 跳过已有产物的 chat
        std::string error;
        std::vector<CrewEvent> events;
        std::vector<Artifact> artifacts;
        std::vector<VoteRecord> votes;
        std::map<std::string, AgentStatus> agents;
        int64_t createdMs = 0;
        int64_t updatedMs = 0;
        bool approved = false;
        std::string approvalComment;
        std::atomic<bool> cancelRequested{false};
        std::atomic<bool> interrupted{false}; // 引擎关停：可恢复中断（区别于用户取消）
        std::atomic<bool> humanBallotSet{false};
        std::string humanBallot;
        std::map<std::string, RoleUsage> usage;
        std::map<std::string, int> roleCalls; // 剧本测试按调用序区分轮次
        std::string lastIssues;               // 缺陷环：上轮结构化 issue 清单
    };
    using TaskPtr = std::shared_ptr<TaskState>;

    struct RoleDef {
        std::string role;
        std::string prompt;
        int voteWeight = 1;
    };

    // ---- 编排 ----
    void runPipeline(TaskPtr t);
    bool phaseImpl(TaskPtr t);          // story 子环并行；false=回退整片实现
    void phaseTestBugfix(TaskPtr t);    // Test + Bugfix（结构化 issue 环）
    [[nodiscard]] json::JsonValue buildScorecard(const TaskPtr& t);
    [[nodiscard]] std::vector<std::string> voterListWithHuman();
    [[nodiscard]] bool checkCancel(const TaskPtr& t); // true = 已取消（置终态）

    // ---- 基础设施 ----
    void emit(TaskPtr t, CrewPhase phase, const std::string& agent,
              const std::string& type, const std::string& detail,
              const std::string& dataJson = {});
    [[nodiscard]] std::string chat(TaskPtr t, const std::string& role,
                                   const std::string& userPrompt);
    /// 结构化调用：解析 + 字段校验 + 自修正重试（带错误反馈）；
    /// 返回最后一次回复的原文（尽力而为语义与 M11 chat() 一致）
    [[nodiscard]] std::string chatJson(TaskPtr t, const std::string& role,
                                       const std::string& userPrompt,
                                       const std::vector<std::string>& requiredFields);
    void saveArtifact(TaskPtr t, const std::string& type, const std::string& agent,
                      const std::string& content);
    [[nodiscard]] std::string latestArtifact(const TaskPtr& t, const std::string& type) const;
    [[nodiscard]] static json::JsonValue extractJson(const std::string& text);
    void recordUsage(const TaskPtr& t, const std::string& role,
                     const ai::ChatResult& r);
    void fireWebhook(const TaskPtr& t, const char* event);
    // 受管线程池
    void enqueue(const TaskPtr& t);
    void workerLoop();
    // 持久化
    void persistTask(const TaskPtr& t);
    void loadPersisted();
    json::JsonValue taskToJson(const TaskPtr& t);
    // 组织记忆（hash 向量 + 余弦暴力检索）
    struct MemoryEntry {
        std::string brief;
        std::string summary;
        std::vector<double> vec;
    };
    void memoryRemember(const std::string& brief, const std::string& summary);
    [[nodiscard]] std::vector<MemoryEntry> memoryRecall(const std::string& brief, size_t k);
    void memoryLoad();
    void memorySave();

    EngineOptions opt_;
    std::vector<RoleDef> roles_;
    std::shared_ptr<ai::IInferenceService> inference_;
    std::mutex mtx_;                        // tasks_/subs_/queue_ 保护
    std::map<std::string, TaskPtr> tasks_;
    uint64_t subSeq_ = 1;
    std::map<uint64_t, std::pair<std::string, EventCallback>> subs_;
    std::atomic<uint64_t> taskSeq_{0};
    // 线程池
    std::vector<TaskPtr> queue_;
    std::vector<std::thread> workers_;
    std::atomic<bool> poolRunning_{false};
    std::condition_variable poolCv_;
    // 组织记忆
    std::vector<MemoryEntry> memory_;
};

} // namespace zenith::crew
