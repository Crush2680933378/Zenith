// ============================================================================
//  CrewEngine 实现（M11 基线 + M14 全面强化）
// ============================================================================
#include "zenith/crew/crew.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>

#include "zenith/ai/provider_router.hpp" // simpleHttpRequest（webhook）
#include "zenith/http/otel.hpp"

namespace zenith::crew {
namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// 取消信号异常（runPipeline 捕获 → Cancelled 终态）
struct CrewCancelled {};
/// 引擎关停中断（可恢复：持久化为 interrupted，重启后 resumable）
struct CrewInterrupted {};

/// 默认角色种子（提示词含 "ROLE: X" 标记——剧本推理按此分发，勿改标记）
struct RoleDefSeed {
    const char* role;
    const char* prompt;
    int voteWeight;
    const char* display;
};
const std::vector<RoleDefSeed>& defaultRoles() {
    static const std::vector<RoleDefSeed> kRoles = {
        {"pm", "ROLE: PM. You are the product manager. Given a brief, produce ONLY one JSON "
               "object: {\"prd\":{\"summary\":\"...\",\"stories\":[{\"id\":\"S1\",\"title\":"
               "\"...\",\"given\":\"...\",\"when\":\"...\",\"then\":\"...\"}]}}. Every story "
               "must have executable Given/When/Then acceptance criteria.", 1, "产品经理"},
        {"architect", "ROLE: ARCHITECT. You are the software architect. Given the PRD, "
                      "produce ONLY one JSON object: {\"architecture\":{\"style\":\"...\","
                      "\"storage\":\"sqlite|postgres|none\",\"components\":[\"...\"],"
                      "\"api\":[{\"method\":\"GET\",\"path\":\"/api/...\",\"desc\":\"...\"}],"
                      "\"notes\":\"...\"}}. Contract-first: api entries are the single "
                      "source of truth for backend and frontend.", 2, "架构师"},
        {"backend", "ROLE: BACKEND. You are the backend developer. Given the API contract, "
                    "produce ONLY one JSON object: {\"files\":[{\"path\":\"src/main.cpp\","
                    "\"content\":\"// implementation\"}],\"notes\":\"...\"}. Implement every "
                    "contracted endpoint.", 1, "后端工程师"},
        {"frontend", "ROLE: FRONTEND. You are the frontend developer. Given the API "
                     "contract, produce ONLY one JSON object: {\"files\":[{\"path\":"
                     "\"web/index.html\",\"content\":\"<!-- UI -->\"}],\"notes\":\"...\"}. "
                     "Develop against the contract (mock-friendly), never block on backend.", 1, "前端工程师"},
        {"tester", "ROLE: TESTER. You are the QA engineer. Given the PRD acceptance "
                   "criteria and API contract, produce ONLY one JSON object: {\"cases\":"
                   "[{\"id\":\"T1\",\"story\":\"S1\",\"given\":\"...\",\"when\":\"...\","
                   "\"then\":\"...\"}],\"results\":[{\"case\":\"T1\",\"status\":"
                   "\"pass|fail\",\"detail\":\"...\",\"component\":\"backend|frontend\"}]}. "
                   "Every story needs at least one case. When asked to RETEST after fixes, "
                   "report all cases pass unless the fix note says otherwise.", 1, "测试工程师"},
        {"devops", "ROLE: DEVOPS. You are the release engineer. Given the architecture, "
                   "produce ONLY one JSON object: {\"files\":[{\"path\":\"Dockerfile\","
                   "\"content\":\"...\"},{\"path\":\".ci/pipeline.yml\",\"content\":\"...\"}],"
                   "\"deploy\":{\"strategy\":\"...\",\"smoke\":\"...\"}}.", 1, "运维工程师"},
        {"delivery", "ROLE: DELIVERY. You are the release manager. Given the PRD and test "
                     "outcomes, produce ONLY one JSON object: {\"changelog\":\"...\","
                     "\"release_notes\":\"...\",\"version\":\"0.1.0\"}.", 1, "交付经理"},
    };
    return kRoles;
}

/// 组织记忆向量：64 维词哈希袋（小写分词 → L2 归一）
std::vector<double> hashVec(const std::string& text) {
    std::vector<double> v(64, 0.0);
    std::string word;
    auto flush = [&] {
        if (word.empty()) return;
        size_t h = 1469598103934665603ULL;
        for (char wc : word) h = (h ^ static_cast<unsigned char>(wc)) * 1099511628211ULL;
        v[h % 64] += 1.0;
        word.clear();
    };
    for (char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word.push_back(
                static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            flush();
        }
    }
    flush();
    double norm = 0;
    for (double d : v) norm += d * d;
    norm = std::sqrt(norm);
    if (norm > 0) {
        for (double& d : v) d /= norm;
    }
    return v;
}

double cosine(const std::vector<double>& a, const std::vector<double>& b) {
    double s = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i) s += a[i] * b[i];
    return s;
}

/// 真执行验证（execVerify）：跑命令取输出；exit!=0 视为失败
bool runCommand(const std::string& cmd, std::string& output) {
    output.clear();
#if defined(_WIN32)
    FILE* p = _popen(cmd.c_str(), "r");
#else
    FILE* p = popen(cmd.c_str(), "r");
#endif
    if (p == nullptr) return false;
    char buf[4096];
    while (fgets(buf, sizeof(buf), p) != nullptr) output += buf;
#if defined(_WIN32)
    const int rc = _pclose(p);
#else
    const int rc = pclose(p);
#endif
    return rc == 0;
}

json::JsonValue usageToJson(const RoleUsage& u) {
    json::JsonValue j = json::JsonValue::object();
    j["calls"] = static_cast<int64_t>(u.calls);
    j["tokens_in"] = static_cast<int64_t>(u.tokensIn);
    j["tokens_out"] = static_cast<int64_t>(u.tokensOut);
    j["cost_usd"] = u.costUsd;
    return j;
}

const std::vector<std::pair<const char*, const char*>>& kPersonaRules() {
    // 顺序即优先级；子串小写匹配
    static const std::vector<std::pair<const char*, const char*>> kRules = {
        {"deepseek", "deepseek"}, {"moonshot", "kimi"},     {"kimi", "kimi"},
        {"chatglm", "glm"},       {"zhipu", "glm"},          {"glm", "glm"},
        {"mimo", "mimo"},         {"xiaomi", "mimo"},
        {"minimax", "minimax"},   {"abab", "minimax"},       {"mini-max", "minimax"},
        {"llama", "llama"},       {"qwen", "qwen"},          {"qwq", "qwen"},
        {"gemini", "gemini"},     {"claude", "anthropic"},   {"anthropic", "anthropic"},
        {"gpt", "openai"},        {"o1", "openai"},          {"o3", "openai"},
    };
    return kRules;
}

} // namespace

std::string personaFromModel(const std::string& model) {
    std::string low;
    low.reserve(model.size());
    for (char c : model) {
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    for (const auto& [needle, persona] : kPersonaRules()) {
        if (low.find(needle) != std::string::npos) return persona;
    }
    return "";
}

namespace {

CrewPhase phaseFromName(const std::string& s) {
    for (int i = 0; i <= static_cast<int>(CrewPhase::Cancelled); ++i) {
        if (s == phaseName(static_cast<CrewPhase>(i))) return static_cast<CrewPhase>(i);
    }
    return CrewPhase::Done;
}

} // namespace

// ---------------------------------------------------------------------------
// JSON 序列化
// ---------------------------------------------------------------------------
json::JsonValue CrewEvent::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["seq"] = static_cast<int64_t>(seq);
    v["ts_ms"] = tsMs;
    v["task_id"] = taskId;
    v["phase"] = phaseName(phase);
    v["agent"] = agent;
    v["type"] = type;
    v["detail"] = detail;
    if (!dataJson.empty()) {
        auto d = json::JsonValue::parse(dataJson);
        v["data"] = d ? std::move(d).value() : json::JsonValue(dataJson);
    }
    return v;
}

json::JsonValue Artifact::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["id"] = id;
    v["task_id"] = taskId;
    v["type"] = type;
    v["agent"] = agent;
    v["version"] = version;
    v["content"] = content;
    v["ts_ms"] = tsMs;
    return v;
}

json::JsonValue VoteRecord::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["id"] = static_cast<int64_t>(id);
    v["task_id"] = taskId;
    v["question"] = question;
    json::JsonValue opts = json::JsonValue::array();
    for (const std::string& o : options) opts.push_back(o);
    v["options"] = std::move(opts);
    json::JsonValue vs = json::JsonValue::array();
    for (const auto& [agent, choice] : votes) {
        json::JsonValue e = json::JsonValue::object();
        e["agent"] = agent;
        e["choice"] = choice;
        vs.push_back(std::move(e));
    }
    v["votes"] = std::move(vs);
    v["winner"] = winner;
    v["reason"] = reason;
    v["ts_ms"] = tsMs;
    return v;
}

json::JsonValue AgentStatus::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["role"] = role;
    v["state"] = state;
    v["detail"] = detail;
    v["updated_ms"] = updatedMs;
    return v;
}

json::JsonValue CrewTaskInfo::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["id"] = id;
    v["brief"] = brief;
    v["phase"] = phaseName(phase);
    v["running"] = running;
    v["resumable"] = resumable;
    v["error"] = error;
    json::JsonValue arr = json::JsonValue::array();
    for (const AgentStatus& a : agents) arr.push_back(a.toJson());
    v["agents"] = std::move(arr);
    v["event_count"] = static_cast<int64_t>(eventCount);
    v["artifact_count"] = static_cast<int64_t>(artifactCount);
    v["vote_count"] = static_cast<int64_t>(voteCount);
    v["created_ms"] = createdMs;
    v["updated_ms"] = updatedMs;
    v["cost_usd"] = costUsd;
    v["tokens_in"] = static_cast<int64_t>(tokensIn);
    v["tokens_out"] = static_cast<int64_t>(tokensOut);
    json::JsonValue ru = json::JsonValue::object();
    for (const auto& [role, u] : roleUsage) ru[role] = usageToJson(u);
    v["role_usage"] = std::move(ru);
    json::JsonValue rm = json::JsonValue::object();
    for (const auto& [role, m] : roleModels) rm[role] = m;
    v["role_models"] = std::move(rm);
    v["config"] = cfg.toJson();
    return v;
}

json::JsonValue CrewConfig::toJson() const {
    json::JsonValue v = json::JsonValue::object();
    v["model"] = model;
    json::JsonValue rm = json::JsonValue::object();
    for (const auto& [r, m] : roleModels) rm[r] = m;
    v["role_models"] = std::move(rm);
    v["max_bug_cycles"] = maxBugCycles;
    v["require_approval"] = requireApproval;
    v["approval_timeout_sec"] = approvalTimeoutSec;
    v["parallel_implement"] = parallelImplement;
    v["strict_retest"] = strictRetest;
    v["json_retry_limit"] = jsonRetryLimit;
    v["stream_tokens"] = streamTokens;
    v["exec_verify"] = execVerify;
    v["use_memory"] = useMemory;
    v["stories_mode"] = storiesMode;
    v["prd_vote_rounds"] = prdVoteRounds;
    v["human_voter"] = humanVoter;
    v["human_vote_timeout_sec"] = humanVoteTimeoutSec;
    return v;
}

CrewConfig CrewConfig::fromJson(const json::JsonValue& v) {
    CrewConfig c;
    c.model = v["model"].asString();
    if (const auto* rm = v["role_models"].asObject()) {
        for (const auto& [r, m] : *rm) c.roleModels[r] = m.asString();
    }
    if (const auto* rm2 = v["roleModels"].asObject()) { // camelCase 兼容（HTTP 层）
        for (const auto& [r, m] : *rm2) c.roleModels[r] = m.asString();
    }
    c.maxBugCycles = static_cast<int>(
        v["max_bug_cycles"].asInt(v["maxBugCycles"].asInt(2)));
    c.requireApproval =
        v["require_approval"].asBool(v["requireApproval"].asBool(false));
    c.approvalTimeoutSec = static_cast<int>(
        v["approval_timeout_sec"].asInt(v["approvalTimeoutSec"].asInt(3600)));
    c.parallelImplement =
        v["parallel_implement"].asBool(v["parallelImplement"].asBool(true));
    c.strictRetest = v["strict_retest"].asBool(v["strictRetest"].asBool(true));
    c.jsonRetryLimit = static_cast<int>(
        v["json_retry_limit"].asInt(v["jsonRetryLimit"].asInt(2)));
    c.streamTokens = v["stream_tokens"].asBool(v["streamTokens"].asBool(true));
    c.execVerify = v["exec_verify"].asBool(v["execVerify"].asBool(false));
    c.useMemory = v["use_memory"].asBool(v["useMemory"].asBool(true));
    c.storiesMode = v["stories_mode"].asBool(v["storiesMode"].asBool(false));
    c.prdVoteRounds = static_cast<int>(
        v["prd_vote_rounds"].asInt(v["prdVoteRounds"].asInt(1)));
    c.humanVoter = v["human_voter"].asBool(v["humanVoter"].asBool(false));
    c.humanVoteTimeoutSec = static_cast<int>(
        v["human_vote_timeout_sec"].asInt(v["humanVoteTimeoutSec"].asInt(600)));
    return c;
}

const char* phaseName(CrewPhase p) noexcept {
    switch (p) {
        case CrewPhase::Pending: return "pending";
        case CrewPhase::Requirements: return "requirements";
        case CrewPhase::ReviewPrd: return "review_prd";
        case CrewPhase::Design: return "design";
        case CrewPhase::Implement: return "implement";
        case CrewPhase::Test: return "test";
        case CrewPhase::Bugfix: return "bugfix";
        case CrewPhase::Deploy: return "deploy";
        case CrewPhase::Release: return "release";
        case CrewPhase::AwaitingApproval: return "awaiting_approval";
        case CrewPhase::Done: return "done";
        case CrewPhase::Failed: return "failed";
        case CrewPhase::Cancelled: return "cancelled";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// CrewEngine：生命周期
// ---------------------------------------------------------------------------
CrewEngine::CrewEngine() {
    setOptions(EngineOptions{});
}

CrewEngine::CrewEngine(EngineOptions opt) {
    setOptions(std::move(opt));
}

CrewEngine::~CrewEngine() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& [id, t] : tasks_) {
            if (t->running) {
                t->interrupted.store(true); // 协作式中断（可恢复，区别于用户取消）
                t->error = "engine shutdown";
            }
        }
    }
    poolRunning_.store(false);
    poolCv_.notify_all();
    for (std::thread& w : workers_) {
        if (w.joinable()) w.join();
    }
}

void CrewEngine::setOptions(EngineOptions opt) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        opt_ = std::move(opt);
        // 角色集：配置覆盖（role/prompt/voteWeight）或默认 7 角色
        roles_.clear();
        if (!opt_.rolesJson.empty()) {
            auto v = json::JsonValue::parse(opt_.rolesJson);
            if (v && v.value().isArray()) {
                for (const json::JsonValue& r : *v.value().asArray()) {
                    RoleDef d;
                    d.role = r["role"].asString();
                    d.prompt = r["prompt"].asString();
                    d.voteWeight = static_cast<int>(r["voteWeight"].asInt(1));
                    d.display = r["display"].asString();
                    if (!d.role.empty() && !d.prompt.empty()) {
                        roles_.push_back(std::move(d));
                    }
                }
            }
        }
        if (roles_.empty()) {
            for (const RoleDefSeed& s : defaultRoles()) {
                RoleDef d;
                d.role = s.role;
                d.prompt = s.prompt;
                d.voteWeight = s.voteWeight;
                d.display = s.display;
                roles_.push_back(std::move(d));
            }
        }
        // 形象覆盖：{"pm":"glm","backend":"kimi"}（roster 输出给前端选立绘）
        personas_.clear();
        if (!opt_.personasJson.empty()) {
            auto pv = json::JsonValue::parse(opt_.personasJson);
            if (pv && pv.value().isObject()) {
                for (const auto& [role, persona] : *pv.value().asObject()) {
                    personas_[role] = persona.asString();
                }
            }
        }
    }
    if (!opt_.persistDir.empty()) { // 锁外加载（内部自锁）
        memoryLoad();
        loadPersisted();
    }
}

void CrewEngine::setInference(std::shared_ptr<ai::IInferenceService> inf) {
    std::lock_guard<std::mutex> lk(mtx_);
    inference_ = std::move(inf);
}

// ---------------------------------------------------------------------------
// 事件与产物
// ---------------------------------------------------------------------------
void CrewEngine::emit(TaskPtr t, CrewPhase phase, const std::string& agent,
                      const std::string& type, const std::string& detail,
                      const std::string& dataJson) {
    CrewEvent e;
    e.tsMs = nowMs();
    e.taskId = t->id;
    e.phase = phase;
    e.agent = agent;
    e.type = type;
    e.detail = detail;
    e.dataJson = dataJson;

    std::vector<EventCallback> toNotify;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        e.seq = t->events.empty() ? 1 : t->events.back().seq + 1;
        t->events.push_back(e);
        if (t->events.size() > 4096) {
            t->events.erase(
                t->events.begin(),
                t->events.begin() + static_cast<long>(t->events.size() - 4096));
        }
        t->updatedMs = e.tsMs;
        for (auto& [id, sub] : subs_) {
            if (sub.first == t->id) toNotify.push_back(sub.second);
        }
    }
    for (const EventCallback& cb : toNotify) cb(e); // 锁外触发
}

void CrewEngine::emitHandoff(const TaskPtr& t, const std::string& from,
                             const std::string& to, const std::string& item) {
    json::JsonValue d = json::JsonValue::object();
    d["from"] = from;
    d["to"] = to;
    d["item"] = item;
    emit(t, t->phase, from, "handoff", from + " → " + to + " (" + item + ")", d.dump());
}

json::JsonValue CrewEngine::extractJson(const std::string& text) {
    const size_t b = text.find('{');
    const size_t e = text.rfind('}');
    if (b == std::string::npos || e == std::string::npos || e <= b) {
        return json::JsonValue();
    }
    auto v = json::JsonValue::parse(text.substr(b, e - b + 1));
    return v ? std::move(v).value() : json::JsonValue();
}

void CrewEngine::recordUsage(const TaskPtr& t, const std::string& role,
                             const ai::ChatResult& r) {
    std::lock_guard<std::mutex> lk(mtx_);
    RoleUsage& u = t->usage[role];
    ++u.calls;
    const int64_t in = r.usage.promptTokens;
    const int64_t out = r.usage.completionTokens;
    u.tokensIn += in > 0 ? static_cast<uint64_t>(in) : 0;
    u.tokensOut += out > 0 ? static_cast<uint64_t>(out) : 0;
    if (opt_.pricePromptPerM > 0 || opt_.priceCompletionPerM > 0) {
        u.costUsd += static_cast<double>(in > 0 ? in : 0) / 1e6 * opt_.pricePromptPerM +
                     static_cast<double>(out > 0 ? out : 0) / 1e6 *
                         opt_.priceCompletionPerM;
    }
}

void CrewEngine::saveArtifact(TaskPtr t, const std::string& type, const std::string& agent,
                              const std::string& content) {
    Artifact a;
    a.taskId = t->id;
    a.type = type;
    a.agent = agent;
    a.content = content;
    a.tsMs = nowMs();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int ver = 0;
        for (const Artifact& x : t->artifacts) {
            if (x.type == type) ver = std::max(ver, x.version);
        }
        a.version = ver + 1;
        a.id = type + "-v" + std::to_string(a.version);
        t->artifacts.push_back(a);
    }
    json::JsonValue d = json::JsonValue::object();
    d["artifact"] = a.id;
    d["bytes"] = static_cast<int64_t>(content.size());
    emit(t, t->phase, agent, "artifact", a.id + " saved (" + type + ")", d.dump());
    persistTask(t);
}

std::string CrewEngine::latestArtifact(const TaskPtr& t, const std::string& type) const {
    for (size_t i = t->artifacts.size(); i-- > 0;) {
        if (t->artifacts[i].type == type) return t->artifacts[i].content;
    }
    return {};
}

// ---------------------------------------------------------------------------
// 推理调用（流式 token / 每角色模型 / 重试 / OTel / 成本）
// ---------------------------------------------------------------------------
std::string CrewEngine::chat(TaskPtr t, const std::string& role,
                             const std::string& userPrompt) {
    if (t->interrupted.load()) throw CrewInterrupted{};
    if (t->cancelRequested.load()) throw CrewCancelled{};

    std::shared_ptr<ai::IInferenceService> inf;
    RoleDef def;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        inf = inference_;
        for (const RoleDef& r : roles_) {
            if (role == r.role) def = r;
        }
    }
    if (inf == nullptr) throw std::runtime_error("no inference service");
    if (def.role.empty()) throw std::runtime_error("unknown role: " + role);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        t->roleCalls[role]++; // 剧本测试按调用序区分轮次
        auto& st = t->agents[role];
        st.role = role;
        st.state = "working";
        st.detail = userPrompt.substr(0, std::min<size_t>(userPrompt.size(), 80));
        st.updatedMs = nowMs();
    }
    emit(t, t->phase, role, "agent_start", def.role + " working");

    obs::OtelExporter* otel = opt_.otel.get();
    bool haveSpan = otel != nullptr;
    obs::OtelSpan span = haveSpan ? otel->beginSpan("crew." + role) : obs::OtelSpan{};

    ai::ChatRequest req;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = t->cfg.roleModels.find(role); // 每角色模型覆盖 → 任务默认
        req.model = it != t->cfg.roleModels.end() ? it->second : t->cfg.model;
    }
    ai::ChatMessage sys;
    sys.role = "system";
    sys.content = def.prompt;
    req.messages.push_back(std::move(sys));
    ai::ChatMessage user;
    user.role = "user";
    user.content = userPrompt;
    req.messages.push_back(std::move(user));

    // token 流入事件流（节流：攒 24 字符一发；mock 不回调则零事件）
    std::string tokBuf;
    const bool wantStream = t->cfg.streamTokens;
    ai::TokenCallback onTok;
    if (wantStream) {
        onTok = [&](std::string_view delta) {
            tokBuf.append(delta.data(), delta.size());
            if (tokBuf.size() >= 24) {
                json::JsonValue d = json::JsonValue::object();
                d["delta"] = tokBuf;
                emit(t, t->phase, role, "token",
                     "+" + std::to_string(tokBuf.size()) + " chars", d.dump());
                tokBuf.clear();
            }
            return true;
        };
    }

    ai::ChatResult r;
    const int maxAttempts = 3; // 瞬时故障重试（failover 已在路由层）
    for (int attempt = 1; attempt <= maxAttempts; ++attempt) {
        r = inf->chat(req, wantStream ? onTok : ai::TokenCallback{});
        if (r.ok || t->cancelRequested.load() || t->interrupted.load()) break;
        if (attempt == maxAttempts) break;
        emit(t, t->phase, role, "retry",
             "inference attempt " + std::to_string(attempt) +
                 " failed: " + r.error.substr(0, 80));
        std::this_thread::sleep_for(std::chrono::milliseconds(100 * attempt));
    }
    if (!tokBuf.empty()) { // 尾片
        json::JsonValue d = json::JsonValue::object();
        d["delta"] = tokBuf;
        emit(t, t->phase, role, "token", "tail", d.dump());
    }
    recordUsage(t, role, r);
    if (haveSpan) {
        otel->endSpan(span, {{"crew.task", t->id},
                             {"crew.role", role},
                             {"crew.phase", phaseName(t->phase)},
                             {"crew.ok", r.ok ? "true" : "false"}});
    }
    if (!r.ok) throw std::runtime_error("inference failed: " + r.error);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto& st = t->agents[role];
        st.state = "done";
        st.updatedMs = nowMs();
    }
    emit(t, t->phase, role, "agent_done", def.role + " done");
    return r.content;
}

std::string CrewEngine::chatJson(TaskPtr t, const std::string& role,
                                 const std::string& userPrompt,
                                 const std::vector<std::string>& requiredFields) {
    const int limit = t->cfg.jsonRetryLimit;
    std::string prompt = userPrompt;
    std::string lastText;
    for (int attempt = 0; attempt <= limit; ++attempt) {
        lastText = chat(t, role, prompt);
        json::JsonValue v = extractJson(lastText);
        bool valid = !v.isNull();
        std::string missing;
        if (valid) {
            for (const std::string& f : requiredFields) {
                if (v[f].isNull()) {
                    missing = f;
                    valid = false;
                    break;
                }
            }
        }
        if (valid) return lastText;
        if (attempt == limit) {
            emit(t, t->phase, role, "gate",
                 "malformed JSON from " + role + " after " +
                     std::to_string(attempt + 1) + " attempt(s); continuing best-effort");
            return lastText; // 尽力而为（保持 M11 容错语义）
        }
        emit(t, t->phase, role, "retry",
             "structured output retry " + std::to_string(attempt + 1) +
                 (missing.empty() ? " (unparseable JSON)"
                                  : " (missing field: " + missing + ")"));
        prompt = userPrompt + "\nYour previous reply was INVALID JSON" +
                 (missing.empty() ? std::string()
                                  : " (missing field \"" + missing + "\")") +
                 ". Reply again with ONLY the corrected JSON object.";
    }
    return lastText;
}

// ---------------------------------------------------------------------------
// 受管线程池（并发上限 + 优雅收尾）
// ---------------------------------------------------------------------------
void CrewEngine::enqueue(const TaskPtr& t) {
    std::unique_lock<std::mutex> lk(mtx_);
    if (!poolRunning_.load()) {
        poolRunning_.store(true);
        const int n = std::max(1, opt_.maxConcurrentTasks);
        workers_.clear();
        for (int i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
    }
    queue_.push_back(t);
    poolCv_.notify_one();
}

void CrewEngine::workerLoop() {
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        while (queue_.empty() && poolRunning_.load()) {
            poolCv_.wait_for(lk, std::chrono::milliseconds(200));
        }
        if (queue_.empty()) return; // 停机且队列空
        TaskPtr t = queue_.front();
        queue_.erase(queue_.begin());
        lk.unlock();
        runPipeline(t);
        lk.lock();
    }
}

// ---------------------------------------------------------------------------
// 组织记忆
// ---------------------------------------------------------------------------
void CrewEngine::memoryRemember(const std::string& brief, const std::string& summary) {
    if (!opt_.memoryEnabled) return;
    MemoryEntry e;
    e.brief = brief.substr(0, 200);
    e.summary = summary.substr(0, 600);
    e.vec = hashVec(brief);
    std::lock_guard<std::mutex> lk(mtx_);
    memory_.push_back(std::move(e));
    if (memory_.size() > 256) memory_.erase(memory_.begin());
    memorySave();
}

std::vector<CrewEngine::MemoryEntry> CrewEngine::memoryRecall(const std::string& brief,
                                                              size_t k) {
    const std::vector<double> q = hashVec(brief);
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::pair<double, size_t>> scored;
    for (size_t i = 0; i < memory_.size(); ++i) {
        const double s = cosine(q, memory_[i].vec);
        if (s > 0.15) scored.emplace_back(s, i);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<MemoryEntry> out;
    for (size_t i = 0; i < scored.size() && i < k; ++i) {
        out.push_back(memory_[scored[i].second]);
    }
    return out;
}

void CrewEngine::memorySave() {
    if (opt_.persistDir.empty()) return; // 调用方持锁
    json::JsonValue arr = json::JsonValue::array();
    for (const MemoryEntry& e : memory_) {
        json::JsonValue j = json::JsonValue::object();
        j["brief"] = e.brief;
        j["summary"] = e.summary;
        arr.push_back(std::move(j));
    }
    json::JsonValue root = json::JsonValue::object();
    root["memory"] = std::move(arr);
    const std::string path = opt_.persistDir + "/crew_memory.json";
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return;
    f << root.dump();
    f.close();
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

void CrewEngine::memoryLoad() {
    const std::string path = opt_.persistDir + "/crew_memory.json";
    std::ifstream f(path, std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    auto v = json::JsonValue::parse(ss.str());
    if (!v) return;
    const auto* arr = v.value()["memory"].asArray();
    if (arr == nullptr) return;
    std::lock_guard<std::mutex> lk(mtx_);
    for (const json::JsonValue& e : *arr) {
        MemoryEntry m;
        m.brief = e["brief"].asString();
        m.summary = e["summary"].asString();
        m.vec = hashVec(m.brief);
        if (!m.brief.empty()) memory_.push_back(std::move(m));
    }
}

// ---------------------------------------------------------------------------
// 持久化（任务快照 + 断点恢复）
// ---------------------------------------------------------------------------
json::JsonValue CrewEngine::taskToJson(const TaskPtr& t) {
    json::JsonValue v = json::JsonValue::object();
    v["id"] = t->id;
    v["brief"] = t->brief;
    v["cfg"] = t->cfg.toJson();
    v["phase"] = phaseName(t->phase);
    v["running"] = t->running;
    v["interrupted"] = t->interrupted.load();
    v["error"] = t->error;
    v["created_ms"] = t->createdMs;
    v["updated_ms"] = t->updatedMs;
    json::JsonValue arts = json::JsonValue::array();
    for (const Artifact& a : t->artifacts) arts.push_back(a.toJson());
    v["artifacts"] = std::move(arts);
    json::JsonValue vs = json::JsonValue::array();
    for (const VoteRecord& vr : t->votes) vs.push_back(vr.toJson());
    v["votes"] = std::move(vs);
    json::JsonValue evs = json::JsonValue::array();
    for (const CrewEvent& e : t->events) evs.push_back(e.toJson());
    v["events"] = std::move(evs);
    json::JsonValue usage = json::JsonValue::object();
    for (const auto& [role, u] : t->usage) usage[role] = usageToJson(u);
    v["usage"] = std::move(usage);
    return v;
}

void CrewEngine::persistTask(const TaskPtr& t) {
    if (opt_.persistDir.empty()) return;
    const std::string path = opt_.persistDir + "/" + t->id + ".json";
    const std::string tmp = path + ".tmp";
    std::ofstream f(tmp, std::ios::binary);
    if (!f) return;
    f << taskToJson(t).dump();
    f.close();
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}

void CrewEngine::loadPersisted() {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (!fs::exists(opt_.persistDir, ec)) return;
    for (const auto& entry : fs::directory_iterator(opt_.persistDir, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.size() < 6 || name.compare(name.size() - 5, 5, ".json") != 0) continue;
        if (name == "crew_memory.json") continue;
        std::ifstream f(entry.path(), std::ios::binary);
        if (!f) continue;
        std::stringstream ss;
        ss << f.rdbuf();
        auto v = json::JsonValue::parse(ss.str());
        if (!v) continue;
        const json::JsonValue& j = v.value();
        TaskPtr t = std::make_shared<TaskState>();
        t->id = j["id"].asString();
        t->brief = j["brief"].asString();
        t->cfg = CrewConfig::fromJson(j["cfg"]);
        t->error = j["error"].asString();
        t->createdMs = j["created_ms"].asInt(0);
        t->updatedMs = j["updated_ms"].asInt(0);
        t->phase = phaseFromName(j["phase"].asString());
        if (const auto* arts = j["artifacts"].asArray()) {
            for (const json::JsonValue& a : *arts) {
                Artifact x;
                x.id = a["id"].asString();
                x.taskId = a["task_id"].asString();
                x.type = a["type"].asString();
                x.agent = a["agent"].asString();
                x.version = static_cast<int>(a["version"].asInt(1));
                x.content = a["content"].asString();
                x.tsMs = a["ts_ms"].asInt(0);
                t->artifacts.push_back(std::move(x));
            }
        }
        if (const auto* vs = j["votes"].asArray()) {
            for (const json::JsonValue& vr : *vs) {
                VoteRecord x;
                x.id = static_cast<uint64_t>(vr["id"].asInt(0));
                x.taskId = vr["task_id"].asString();
                x.question = vr["question"].asString();
                if (const auto* os = vr["options"].asArray()) {
                    for (const auto& o : *os) x.options.push_back(o.asString());
                }
                if (const auto* vv = vr["votes"].asArray()) {
                    for (const auto& e2 : *vv) {
                        x.votes.emplace_back(e2["agent"].asString(),
                                             e2["choice"].asString());
                    }
                }
                x.winner = vr["winner"].asString();
                x.reason = vr["reason"].asString();
                x.tsMs = vr["ts_ms"].asInt(0);
                t->votes.push_back(std::move(x));
            }
        }
        if (const auto* evs = j["events"].asArray()) {
            for (const json::JsonValue& e : *evs) {
                CrewEvent x;
                x.seq = static_cast<uint64_t>(e["seq"].asInt(0));
                x.tsMs = e["ts_ms"].asInt(0);
                x.taskId = e["task_id"].asString();
                x.agent = e["agent"].asString();
                x.type = e["type"].asString();
                x.detail = e["detail"].asString();
                x.phase = phaseFromName(e["phase"].asString());
                t->events.push_back(std::move(x));
            }
        }
        {
            std::vector<RoleDef> roles;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                roles = roles_;
            }
            for (const RoleDef& r : roles) {
                AgentStatus st;
                st.role = r.role;
                st.state = "idle";
                t->agents[r.role] = st;
            }
        }
        const bool wasRunning = j["running"].asBool(false);
        const bool wasInterrupted = j["interrupted"].asBool(false);
        const bool terminal = t->phase == CrewPhase::Done ||
                              t->phase == CrewPhase::Failed ||
                              t->phase == CrewPhase::Cancelled;
        t->running = false;
        t->interrupted.store(false);
        t->resumable = (wasRunning || wasInterrupted) && !terminal;
        if (t->resumable) t->error = "interrupted by restart";
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_[t->id] = t;
    }
}

// ---------------------------------------------------------------------------
// Webhook
// ---------------------------------------------------------------------------
void CrewEngine::fireWebhook(const TaskPtr& t, const char* event) {
    if (opt_.webhookUrl.empty()) return;
    json::JsonValue v = json::JsonValue::object();
    v["event"] = event;
    v["task_id"] = t->id;
    v["phase"] = phaseName(t->phase);
    v["brief"] = t->brief.substr(0, 120);
    ai::SimpleHttpCall call;
    call.method = "POST";
    call.url = opt_.webhookUrl;
    call.body = v.dump();
    call.headers = {{"Content-Type", "application/json"}};
    call.timeoutMs = 5000;
    const bool ok = ai::simpleHttpRequest(call).ok; // 结果忽略（事件可查）
    json::JsonValue d = json::JsonValue::object();
    d["target"] = opt_.webhookUrl;
    d["delivered"] = ok;
    emit(t, t->phase, "orchestrator", "webhook",
         std::string(event) + std::string(ok ? " delivered" : " failed"), d.dump());
}

// ---------------------------------------------------------------------------
// 编排
// ---------------------------------------------------------------------------
bool CrewEngine::checkCancel(const TaskPtr& t) {
    if (t->interrupted.load()) throw CrewInterrupted{};
    if (!t->cancelRequested.load()) return false;
    t->phase = CrewPhase::Cancelled;
    t->error = "cancelled by user";
    emit(t, t->phase, "orchestrator", "cancelled", "task cancelled");
    persistTask(t);
    fireWebhook(t, "cancelled");
    return true;
}

std::string CrewEngine::createTask(const std::string& brief, const CrewConfig& cfg) {
    TaskPtr t = std::make_shared<TaskState>();
    t->brief = brief;
    t->cfg = cfg;
    t->createdMs = nowMs();
    t->updatedMs = t->createdMs;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (inference_ == nullptr) {
            throw std::runtime_error("no inference service configured");
        }
        t->id = "crew-" + std::to_string(taskSeq_.fetch_add(1) + 1) + "-" +
                std::to_string(static_cast<uint32_t>(nowMs() % 100000));
        for (const RoleDef& r : roles_) {
            AgentStatus st;
            st.role = r.role;
            st.state = "idle";
            st.updatedMs = t->createdMs;
            t->agents[r.role] = st;
        }
        tasks_[t->id] = t;
    }
    t->running = true;
    emit(t, CrewPhase::Pending, "orchestrator", "phase",
         "task created: " + brief.substr(0, 60));
    enqueue(t);
    return t->id;
}

void CrewEngine::runPipeline(TaskPtr t) {
    try {
        // ---- 需求（resume：已有 prd 直接复用；组织记忆注入）----
        t->phase = CrewPhase::Requirements;
        emit(t, t->phase, "orchestrator", "phase", "enter requirements");
        std::string pmPrompt = "Product brief: " + t->brief + "\nProduce the PRD JSON now.";
        if (t->cfg.useMemory && opt_.memoryEnabled) {
            auto hits = memoryRecall(t->brief, 2);
            if (!hits.empty()) {
                std::string lessons;
                for (const MemoryEntry& m : hits) {
                    if (!lessons.empty()) lessons += "\n";
                    lessons += "- " + m.summary;
                }
                pmPrompt = "Past project lessons (from crew memory):\n" + lessons +
                           "\n\n" + pmPrompt;
                json::JsonValue d = json::JsonValue::object();
                d["hits"] = static_cast<int64_t>(hits.size());
                emit(t, t->phase, "orchestrator", "memory",
                     "recalled " + std::to_string(hits.size()) + " past lesson(s)",
                     d.dump());
            }
        }
        std::string prd = (t->resumeMode && !latestArtifact(t, "prd").empty())
                              ? latestArtifact(t, "prd")
                              : chatJson(t, "pm", pmPrompt, {"prd"});
        saveArtifact(t, "prd", "pm", prd);

        // ---- PRD 评审（可多轮辩论；prdVoteRounds=修订预算，默认 1 与 M11 一致）----
        t->phase = CrewPhase::ReviewPrd;
        emit(t, t->phase, "orchestrator", "phase", "PRD review vote");
        {
            for (int round = 1;; ++round) {
                VoteSpec spec;
                spec.question =
                    "Does the PRD meet the brief with executable acceptance criteria?";
                spec.options = {"approve", "revise"};
                if (t->cfg.humanVoter) spec.voters = voterListWithHuman();
                auto rec = runVote(t->id, spec);
                if (rec.winner != "revise") break; // approve → 过
                json::JsonValue dr = json::JsonValue::object();
                dr["round"] = round;
                dr["budget"] = t->cfg.prdVoteRounds;
                emit(t, t->phase, "orchestrator", "debate_round",
                     "PRD revision round " + std::to_string(round), dr.dump());
                prd = chatJson(t, "pm",
                               "The crew voted to revise. Improve acceptance criteria. "
                               "Produce the updated PRD JSON now.",
                               {"prd"});
                saveArtifact(t, "prd", "pm", prd);
                if (round >= t->cfg.prdVoteRounds) {
                    emit(t, t->phase, "orchestrator", "gate",
                         "debate budget exhausted (" + std::to_string(round) +
                             " revisions); proceeding with latest PRD");
                    break;
                }
            }
        }

        // ---- 设计（含技术选型投票）----
        t->phase = CrewPhase::Design;
        emit(t, t->phase, "orchestrator", "phase", "enter design");
        {
            VoteSpec spec;
            spec.question = "Primary storage choice for this project?";
            spec.options = {"sqlite", "postgres", "none"};
            if (t->cfg.humanVoter) spec.voters = voterListWithHuman();
            auto rec = runVote(t->id, spec);
            emit(t, t->phase, "orchestrator", "vote",
                 "storage decision: " + rec.winner, rec.toJson().dump());
        }
        emitHandoff(t, "pm", "architect", "prd");
        const std::string arch =
            (t->resumeMode && !latestArtifact(t, "architecture").empty())
                ? latestArtifact(t, "architecture")
                : chatJson(t, "architect",
                           "PRD:\n" + prd + "\nProduce the architecture JSON now.",
                           {"architecture"});
        saveArtifact(t, "architecture", "architect", arch);
        const std::string contract =
            (t->resumeMode && !latestArtifact(t, "contract").empty())
                ? latestArtifact(t, "contract")
                : extractJson(arch)["architecture"]["api"].dump();
        saveArtifact(t, "contract", "architect", contract);
        if (checkCancel(t)) return;

        emitHandoff(t, "architect", "backend", "contract");
        emitHandoff(t, "architect", "frontend", "contract");

        // ---- 人工门禁（可选）：设计完成后 ----
        if (t->cfg.requireApproval) {
            t->phase = CrewPhase::AwaitingApproval;
            emit(t, t->phase, "orchestrator", "approval",
                 "design complete; awaiting human approval");
            persistTask(t);
            fireWebhook(t, "awaiting_approval");
            const int timeoutMs = t->cfg.approvalTimeoutSec * 1000;
            for (int i = 0; i < timeoutMs / 100 + 1; ++i) {
                if (t->interrupted.load()) throw CrewInterrupted{};
                if (t->cancelRequested.load()) throw CrewCancelled{};
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (t->approved) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::string comment;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                t->approved = false;
                comment = std::move(t->approvalComment);
                t->approvalComment.clear();
            }
            if (!comment.empty()) {
                json::JsonValue d = json::JsonValue::object();
                d["comment"] = comment;
                emit(t, t->phase, "human", "approval", "approved: " + comment, d.dump());
            }
            emit(t, t->phase, "orchestrator", "approval", "approved; resuming");
        }

        // ---- 实现（story 子环并行 / 后端 ∥ 前端真并行 / 串行）----
        t->phase = CrewPhase::Implement;
        emit(t, t->phase, "orchestrator", "phase",
             "enter implement (backend ∥ frontend)");
        const bool storiesHandled = t->cfg.storiesMode && phaseImpl(t);
        if (!storiesHandled && t->cfg.parallelImplement) {
            std::string beOut, feOut, beErr, feErr;
            std::thread tb([&] {
                try {
                    beOut = (t->resumeMode && !latestArtifact(t, "backend").empty())
                                ? latestArtifact(t, "backend")
                                : chatJson(t, "backend",
                                           "API contract:\n" + contract +
                                               "\nProduce backend files JSON.",
                                           {"files"});
                } catch (const CrewCancelled&) {
                    beErr = "cancelled";
                } catch (const std::exception& e) {
                    beErr = e.what();
                }
            });
            std::thread tf([&] {
                try {
                    feOut = (t->resumeMode && !latestArtifact(t, "frontend").empty())
                                ? latestArtifact(t, "frontend")
                                : chatJson(t, "frontend",
                                           "API contract:\n" + contract +
                                               "\nProduce frontend files JSON.",
                                           {"files"});
                } catch (const CrewCancelled&) {
                    feErr = "cancelled";
                } catch (const std::exception& e) {
                    feErr = e.what();
                }
            });
            tb.join();
            tf.join();
            if (!beErr.empty() || !feErr.empty()) {
                if (beErr == "cancelled" || feErr == "cancelled") throw CrewCancelled{};
                throw std::runtime_error("implement failed — backend: " + beErr +
                                         " frontend: " + feErr);
            }
            saveArtifact(t, "backend", "backend", beOut);
            saveArtifact(t, "frontend", "frontend", feOut);
        } else {
            const std::string be =
                (t->resumeMode && !latestArtifact(t, "backend").empty())
                    ? latestArtifact(t, "backend")
                    : chatJson(t, "backend",
                               "API contract:\n" + contract +
                                   "\nProduce backend files JSON.",
                               {"files"});
            saveArtifact(t, "backend", "backend", be);
            const std::string fe =
                (t->resumeMode && !latestArtifact(t, "frontend").empty())
                    ? latestArtifact(t, "frontend")
                    : chatJson(t, "frontend",
                               "API contract:\n" + contract +
                                   "\nProduce frontend files JSON.",
                               {"files"});
            saveArtifact(t, "frontend", "frontend", fe);
        }

        if (!storiesHandled) {
            emitHandoff(t, "backend", "tester", "implementation");
            emitHandoff(t, "frontend", "tester", "implementation");
        }

        // ---- 测试 + 结构化缺陷环 ----
        phaseTestBugfix(t);
        if (checkCancel(t)) return;

        // ---- 部署 ----
        t->phase = CrewPhase::Deploy;
        emit(t, t->phase, "orchestrator", "phase", "enter deploy");
        emitHandoff(t, "architect", "devops", "architecture");
        const std::string ops =
            (t->resumeMode && !latestArtifact(t, "devops").empty())
                ? latestArtifact(t, "devops")
                : chatJson(t, "devops",
                           "Architecture:\n" + arch + "\nProduce CI/CD files JSON.",
                           {"files"});
        saveArtifact(t, "devops", "devops", ops);

        // ---- 发布 + 质量评分卡 ----
        t->phase = CrewPhase::Release;
        emit(t, t->phase, "orchestrator", "phase", "enter release");
        emitHandoff(t, "tester", "delivery", "tests");
        const std::string rel =
            (t->resumeMode && !latestArtifact(t, "release").empty())
                ? latestArtifact(t, "release")
                : chatJson(t, "delivery",
                           "PRD:\n" + prd + "\nTests:\n" +
                               std::to_string(t->artifacts.size()) +
                               " artifacts produced. Produce release JSON.",
                           {"changelog"});
        saveArtifact(t, "release", "delivery", rel);
        saveArtifact(t, "scorecard", "orchestrator", buildScorecard(t).dump());

        // ---- 组织记忆归档 ----
        if (t->cfg.useMemory && opt_.memoryEnabled) {
            std::string storage = "n/a";
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (size_t i = t->votes.size(); i-- > 0;) {
                    if (t->votes[i].question.find("storage") != std::string::npos) {
                        storage = t->votes[i].winner;
                        break;
                    }
                }
            }
            json::JsonValue m = json::JsonValue::object();
            m["storage"] = storage;
            m["artifacts"] = static_cast<int64_t>(t->artifacts.size());
            m["verdict"] = "done";
            memoryRemember(t->brief,
                           m.dump() + " (brief: " + t->brief.substr(0, 80) + ")");
        }

        t->phase = CrewPhase::Done;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            t->running = false;
            t->resumable = false;
        }
        emit(t, t->phase, "orchestrator", "phase", "task done");
        persistTask(t);
        fireWebhook(t, "done");
    } catch (const CrewCancelled&) {
        std::lock_guard<std::mutex> lk(mtx_);
        t->phase = CrewPhase::Cancelled;
        t->running = false;
        t->error = "cancelled by user";
    } catch (const CrewInterrupted&) {
        std::lock_guard<std::mutex> lk(mtx_);
        t->running = false;
        t->resumable = true; // 引擎关停 → 重启后可断点续跑
        t->error = "interrupted by engine shutdown";
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        t->phase = CrewPhase::Failed;
        t->running = false;
        t->error = e.what();
    }
    if (t->phase == CrewPhase::Failed || t->phase == CrewPhase::Cancelled) {
        emit(t, t->phase, "orchestrator",
             t->phase == CrewPhase::Failed ? "error" : "cancelled", t->error);
        persistTask(t);
        if (t->phase == CrewPhase::Failed) fireWebhook(t, "failed");
    } else if (t->interrupted.load()) {
        emit(t, t->phase, "orchestrator", "gate", "interrupted; persist for resume");
        persistTask(t);
    }
}

// story 子环：PRD story 列表 → 每 story（后端→测试）并行。
// 返回 false = PRD 无 stories（回退整片实现由调用方处理）
bool CrewEngine::phaseImpl(TaskPtr t) {
    const std::string contract = latestArtifact(t, "contract");
    const json::JsonValue prdJson = extractJson(latestArtifact(t, "prd"));
    const auto* stories = prdJson["prd"]["stories"].asArray();
    if (stories == nullptr || stories->empty()) {
        emit(t, t->phase, "orchestrator", "gate",
             "storiesMode: no stories in PRD; falling back to whole-slice implement");
        return false;
    }
    std::vector<std::string> errs(stories->size());
    std::vector<std::thread> pool;
    for (size_t si = 0; si < stories->size(); ++si) {
        const std::string sid = stories->at(si)["id"].asString();
        const std::string title = stories->at(si)["title"].asString();
        pool.emplace_back([this, t, sid, title, contract, &errs, si] {
            try {
                {
                    json::JsonValue d = json::JsonValue::object();
                    d["story"] = sid;
                    d["title"] = title;
                    emit(t, t->phase, "orchestrator", "story_started",
                         "story " + sid + " started", d.dump());
                }
                const std::string storyArt = "story-" + sid + "-backend";
                const std::string be =
                    (t->resumeMode && !latestArtifact(t, storyArt).empty())
                        ? latestArtifact(t, storyArt)
                        : chatJson(t, "backend",
                                   "STORY " + sid + ": " + title +
                                       "\nAPI contract:\n" + contract +
                                       "\nImplement THIS story. Produce files JSON.",
                                   {"files"});
                saveArtifact(t, storyArt, "backend", be);
                const std::string testArt = "story-" + sid + "-tests";
                const std::string tr = chatJson(
                    t, "tester",
                    "STORY " + sid + ": " + title +
                        "\nVerify THIS story against the implementation notes. "
                        "Produce cases and results JSON (all pass if implemented).",
                    {"cases", "results"});
                saveArtifact(t, testArt, "tester", tr);
                {
                    json::JsonValue d = json::JsonValue::object();
                    d["story"] = sid;
                    d["status"] = "done";
                    emit(t, t->phase, "orchestrator", "story_done",
                         "story " + sid + " done", d.dump());
                }
            } catch (const CrewCancelled&) {
                errs[si] = "cancelled";
            } catch (const std::exception& e) {
                errs[si] = e.what();
            }
        });
    }
    for (std::thread& th : pool) th.join();
    for (const std::string& e : errs) {
        if (e == "cancelled") throw CrewCancelled{};
        if (!e.empty()) throw std::runtime_error("story loop: " + e);
    }
    return true;
}

// 测试 + 结构化 issue 缺陷环
void CrewEngine::phaseTestBugfix(TaskPtr t) {
    const std::string prd = latestArtifact(t, "prd");
    const std::string contract = latestArtifact(t, "contract");
    int cycle = 0;
    for (;;) {
        if (checkCancel(t)) return;
        t->phase = CrewPhase::Test;
        emit(t, t->phase, "orchestrator", "phase",
             cycle == 0 ? "enter test"
                        : "retest after fix (cycle " + std::to_string(cycle) + ")");
        std::string prompt = "PRD:\n" + prd + "\nAPI contract:\n" + contract;
        if (cycle > 0) {
            prompt += t->cfg.strictRetest
                          ? "\nPreviously failing issues:\n" + t->lastIssues +
                                "\nThe responsible developer submitted a fix. RETEST "
                                "now: verify each previously failing case against the "
                                "fix evidence; do NOT assume pass without verification."
                          : "\nPrevious bugs were fixed by backend/frontend. RETEST "
                            "now: mark all previously failing cases as pass.";
        } else {
            prompt += "\nProduce test cases and results JSON now.";
        }
        const std::string tr = chatJson(t, "tester", prompt, {"cases", "results"});
        saveArtifact(t, "tests", "tester", tr);
        const json::JsonValue results = extractJson(tr)["results"];
        const auto* arr = results.asArray();

        // 可选真执行验证：commands[] 逐条跑，exit!=0 → 记失败事件
        if (t->cfg.execVerify) {
            const auto* cmds = extractJson(tr)["commands"].asArray();
            if (cmds != nullptr && !cmds->empty()) {
                for (const json::JsonValue& c : *cmds) {
                    const std::string cmd = c.asString();
                    std::string out;
                    const bool ok = runCommand(cmd, out);
                    emit(t, t->phase, "orchestrator", "exec",
                         cmd + " → " + (ok ? "pass" : "FAIL: " + out.substr(0, 120)));
                }
            }
        }

        bool allPass = true;
        std::string failComponent = "backend";
        std::string issues;
        int passed = 0, failed = 0;
        json::JsonValue failList = json::JsonValue::array();
        if (arr != nullptr) {
            for (const json::JsonValue& r : *arr) {
                if (r["status"].asString() == "fail") {
                    allPass = false;
                    ++failed;
                    if (r["component"].asString() == "frontend") failComponent = "frontend";
                    issues += "- case " + r["case"].asString() + " [" +
                              r["component"].asString() + "]: " +
                              r["detail"].asString() + "\n";
                    json::JsonValue f = json::JsonValue::object();
                    f["case"] = r["case"].asString();
                    f["component"] = r["component"].asString();
                    f["detail"] = r["detail"].asString();
                    failList.push_back(std::move(f));
                } else {
                    ++passed;
                }
            }
        }
        {
            json::JsonValue tr = json::JsonValue::object();
            tr["cycle"] = cycle;
            tr["passed"] = passed;
            tr["failed"] = failed;
            tr["total"] = passed + failed;
            tr["pass_ratio"] = passed + failed > 0
                                   ? static_cast<double>(passed) / (passed + failed)
                                   : 0.0;
            tr["failures"] = failList;
            emit(t, t->phase, "tester", "test_result",
                 "tests: " + std::to_string(passed) + " pass / " +
                     std::to_string(failed) + " fail",
                 tr.dump());
        }
        if (allPass) break;
        if (cycle >= t->cfg.maxBugCycles) {
            emit(t, t->phase, "orchestrator", "gate",
                 "bug budget exhausted; escalating but continuing");
            break;
        }
        t->phase = CrewPhase::Bugfix;
        t->lastIssues = issues;
        {
            json::JsonValue ba = json::JsonValue::object();
            ba["assignee"] = failComponent;
            ba["cycle"] = cycle + 1;
            ba["budget"] = t->cfg.maxBugCycles;
            ba["issues"] = failList;
            emit(t, t->phase, "orchestrator", "bug_assigned",
                 "bugfix: routing to " + failComponent, ba.dump());
        }
        emitHandoff(t, "tester", failComponent, "bugs");
        emit(t, t->phase, "orchestrator", "phase", "bugfix: routing to " + failComponent);
        const std::string fix =
            chatJson(t, failComponent,
                     "QA reported the following failing issues:\n" + issues +
                         "Fix them. Produce updated files JSON.",
                     {"files"});
        saveArtifact(t, "bugs", failComponent, fix);
        ++cycle;
    }
}

// 质量评分卡（启发式：产物覆盖/测试通过率/契约规模/文档）
json::JsonValue CrewEngine::buildScorecard(const TaskPtr& t) {
    json::JsonValue card = json::JsonValue::object();
    size_t testCases = 0, testPass = 0, files = 0, contractEndpoints = 0;
    std::string latestTests, latestContract, latestBe, latestFe, latestRel;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const Artifact& a : t->artifacts) {
            if (a.type == "tests") latestTests = a.content;
            if (a.type == "contract") latestContract = a.content;
            if (a.type == "backend") latestBe = a.content;
            if (a.type == "frontend") latestFe = a.content;
            if (a.type == "release") latestRel = a.content;
        }
    }
    if (const auto* cases = extractJson(latestTests)["cases"].asArray()) {
        testCases = cases->size();
    }
    if (const auto* rs = extractJson(latestTests)["results"].asArray()) {
        for (const json::JsonValue& r : *rs) {
            if (r["status"].asString() == "pass") ++testPass;
        }
    }
    if (auto cj = json::JsonValue::parse(latestContract)) {
        if (const auto* api = cj.value().asArray()) contractEndpoints = api->size();
    }
    if (const auto* fb = extractJson(latestBe)["files"].asArray()) files += fb->size();
    if (const auto* ff = extractJson(latestFe)["files"].asArray()) files += ff->size();

    card["test_cases"] = static_cast<int64_t>(testCases);
    card["test_pass_ratio"] =
        testCases > 0 ? static_cast<double>(testPass) / static_cast<double>(testCases) : 0.0;
    card["contract_endpoints"] = static_cast<int64_t>(contractEndpoints);
    card["files"] = static_cast<int64_t>(files);
    card["artifacts"] = static_cast<int64_t>(t->artifacts.size());
    card["docs_present"] = !latestRel.empty();
    int score = 40; // 基础分：走完全流程
    if (testCases > 0 && testPass == testCases) score += 25;
    if (contractEndpoints > 0) score += 15;
    if (files >= 2) score += 10;
    if (!latestRel.empty()) score += 10;
    card["score"] = score;
    return card;
}

// ---------------------------------------------------------------------------
// 投票
// ---------------------------------------------------------------------------
std::vector<std::string> CrewEngine::voterListWithHuman() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<std::string> out;
    for (const RoleDef& r : roles_) out.push_back(r.role);
    out.push_back("human");
    return out;
}

VoteRecord CrewEngine::runVote(const std::string& taskId, const VoteSpec& specIn) {
    VoteSpec spec = specIn;
    if (spec.voters.empty()) {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const RoleDef& r : roles_) spec.voters.push_back(r.role);
    }

    TaskPtr t;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(taskId);
        if (it == tasks_.end()) throw std::runtime_error("no such task");
        t = it->second;
    }

    VoteRecord rec;
    rec.taskId = taskId;
    rec.question = spec.question;
    rec.options = spec.options;
    rec.tsMs = nowMs();

    std::map<std::string, int> tally;
    std::map<std::string, std::string> reasons;
    for (const std::string& voter : spec.voters) {
        std::string choice;
        std::string reason;
        if (voter == "human") {
            // 人工选民：发事件等待投票（HTTP/引擎入口），超时默认首项
            t->humanBallotSet.store(false);
            {
                std::lock_guard<std::mutex> lk(mtx_);
                t->humanBallot.clear();
            }
            {
                json::JsonValue d = json::JsonValue::object();
                d["question"] = spec.question;
                json::JsonValue os = json::JsonValue::array();
                for (const std::string& o : spec.options) os.push_back(o);
                d["options"] = std::move(os);
                emit(t, t->phase, "human", "human_vote_required",
                     "waiting for human ballot: " + spec.question, d.dump());
            }
            const int timeoutMs = t->cfg.humanVoteTimeoutSec * 1000;
            for (int i = 0; i < timeoutMs / 100 && !t->humanBallotSet.load(); ++i) {
                if (t->interrupted.load()) throw CrewInterrupted{};
                if (t->cancelRequested.load()) throw CrewCancelled{};
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (t->humanBallotSet.load()) {
                std::lock_guard<std::mutex> lk(mtx_);
                choice = t->humanBallot;
                reason = "human ballot";
            } else {
                choice = spec.options.front();
                reason = "[human vote timeout → default]";
            }
        } else {
            std::shared_ptr<ai::IInferenceService> inf;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                inf = inference_;
            }
            if (inf == nullptr) continue;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                auto& st = t->agents[voter];
                st.role = voter;
                st.state = "voting";
                st.updatedMs = nowMs();
            }
            ai::ChatRequest req;
            ai::ChatMessage sys;
            sys.role = "system";
            sys.content = "VOTE: You are the " + voter +
                          " in a software crew. Answer ONLY one JSON object: "
                          "{\"choice\":\"<one of options>\",\"reason\":\"...\"}";
            req.messages.push_back(std::move(sys));
            ai::ChatMessage user;
            user.role = "user";
            user.content = "Question: " + spec.question + "\nOptions:";
            for (const std::string& o : spec.options) user.content += "\n- " + o;
            req.messages.push_back(std::move(user));

            auto r = inf->chat(req);
            if (r.ok) {
                json::JsonValue v = extractJson(r.content);
                choice = v["choice"].asString();
                reason = v["reason"].asString();
            }
        }
        if (std::find(spec.options.begin(), spec.options.end(), choice) ==
            spec.options.end()) {
            choice = spec.options.front(); // 非法票 → 默认首项（记录原始 reason）
            reason = "[invalid vote coerced] " + reason;
        }
        rec.votes.emplace_back(voter, choice);
        reasons[voter] = reason;
        int weight = 1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            for (const RoleDef& rd : roles_) {
                if (rd.role == voter) weight = rd.voteWeight;
            }
        }
        tally[choice] += weight;
        // M15：逐票即时事件（会议动画：依次亮牌）
        json::JsonValue d = json::JsonValue::object();
        d["question"] = spec.question;
        d["choice"] = choice;
        d["reason"] = reason;
        d["weight"] = weight;
        d["human"] = voter == "human";
        emit(t, t->phase, voter, "vote_cast", voter + " → " + choice, d.dump());
    }

    std::string winner;
    int best = -1;
    for (const auto& [opt, n] : tally) {
        if (static_cast<int>(n) > best) {
            best = static_cast<int>(n);
            winner = opt;
        }
    }
    rec.winner = winner;
    for (const auto& [agent, choice] : rec.votes) {
        if (agent == "architect" && choice == winner && !reasons["architect"].empty()) {
            rec.reason = reasons["architect"];
            break;
        }
    }
    if (rec.reason.empty()) {
        rec.reason = "majority (" + std::to_string(best) + " weighted votes)";
    }

    {
        std::lock_guard<std::mutex> lk(mtx_);
        rec.id = t->votes.size() + 1;
        t->votes.push_back(rec);
    }
    emit(t, t->phase, "orchestrator", "vote", spec.question + " → " + rec.winner,
         rec.toJson().dump());
    return rec;
}

// ---------------------------------------------------------------------------
// 服务接口
// ---------------------------------------------------------------------------
core::Result<CrewTaskInfo> CrewEngine::taskInfo(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<CrewTaskInfo>::Err(core::Error(300, "no such task"));
    }
    const TaskPtr& t = it->second;
    CrewTaskInfo info;
    info.id = t->id;
    info.brief = t->brief;
    info.phase = t->phase;
    info.running = t->running;
    info.resumable = t->resumable;
    info.error = t->error;
    info.eventCount = t->events.size();
    info.artifactCount = t->artifacts.size();
    info.voteCount = t->votes.size();
    info.createdMs = t->createdMs;
    info.updatedMs = t->updatedMs;
    info.roleUsage = t->usage;
    info.roleModels = t->cfg.roleModels;
    info.cfg = t->cfg;
    for (const auto& [role, u] : t->usage) {
        info.costUsd += u.costUsd;
        info.tokensIn += u.tokensIn;
        info.tokensOut += u.tokensOut;
    }
    for (const RoleDef& r : roles_) {
        info.agents.push_back(t->agents[r.role]);
    }
    return core::Result<CrewTaskInfo>::Ok(std::move(info));
}

std::vector<CrewTaskInfo> CrewEngine::listTasks() {
    std::lock_guard<std::mutex> lk(mtx_);
    std::vector<CrewTaskInfo> out;
    for (const auto& [id, t] : tasks_) {
        CrewTaskInfo info;
        info.id = t->id;
        info.brief = t->brief.substr(0, 60);
        info.phase = t->phase;
        info.running = t->running;
        info.resumable = t->resumable;
        info.error = t->error;
        info.createdMs = t->createdMs;
        info.updatedMs = t->updatedMs;
        for (const auto& [role, u] : t->usage) info.costUsd += u.costUsd;
        out.push_back(std::move(info));
    }
    return out;
}

std::vector<CrewEvent> CrewEngine::events(const std::string& id, uint64_t sinceSeq) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    std::vector<CrewEvent> out;
    for (const CrewEvent& e : it->second->events) {
        if (e.seq > sinceSeq) out.push_back(e);
    }
    return out;
}

std::vector<Artifact> CrewEngine::artifacts(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    return it->second->artifacts;
}

std::vector<VoteRecord> CrewEngine::votes(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    return it->second->votes;
}

core::Result<void> CrewEngine::approve(const std::string& id) {
    return approve(id, "");
}

core::Result<void> CrewEngine::approve(const std::string& id, const std::string& comment) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<void>::Err(core::Error(300, "no such task"));
    }
    it->second->approved = true;
    it->second->approvalComment = comment;
    return core::Result<void>::Ok();
}

core::Result<void> CrewEngine::cancel(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<void>::Err(core::Error(300, "no such task"));
    }
    if (!it->second->running) {
        return core::Result<void>::Err(core::Error(301, "task not running"));
    }
    it->second->cancelRequested.store(true);
    return core::Result<void>::Ok();
}

core::Result<void> CrewEngine::resume(const std::string& id) {
    TaskPtr t;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = tasks_.find(id);
        if (it == tasks_.end()) {
            return core::Result<void>::Err(core::Error(300, "no such task"));
        }
        t = it->second;
        if (t->running || !t->resumable) {
            return core::Result<void>::Err(core::Error(302, "task not resumable"));
        }
        if (inference_ == nullptr) {
            return core::Result<void>::Err(core::Error(303, "no inference service"));
        }
        t->resumeMode = true;
        t->resumable = false;
        t->running = true;
        t->cancelRequested.store(false);
        t->error.clear(); // approved 保留：审批是用户意图，跨重启有效
    }
    emit(t, t->phase, "orchestrator", "phase",
         "resume from " + std::string(phaseName(t->phase)));
    enqueue(t);
    return core::Result<void>::Ok();
}

core::Result<void> CrewEngine::castHumanVote(const std::string& id,
                                             const std::string& choice) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<void>::Err(core::Error(300, "no such task"));
    }
    it->second->humanBallot = choice;
    it->second->humanBallotSet.store(true);
    return core::Result<void>::Ok();
}

std::string CrewEngine::exportBundle(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) return {};
    return taskToJson(it->second).dump();
}

json::JsonValue CrewEngine::roster(const std::string& taskId) {
    std::vector<RoleDef> roles;
    std::map<std::string, std::string> personas;
    std::map<std::string, std::string> taskModels;
    std::string defaultModel;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        roles = roles_;
        personas = personas_;
        if (!taskId.empty()) {
            auto it = tasks_.find(taskId);
            if (it != tasks_.end()) {
                taskModels = it->second->cfg.roleModels;
                defaultModel = it->second->cfg.model;
            }
        }
    }
    json::JsonValue arr = json::JsonValue::array();
    for (const RoleDef& r : roles) {
        json::JsonValue j = json::JsonValue::object();
        j["role"] = r.role;
        j["display"] = r.display.empty() ? r.role : r.display;
        j["vote_weight"] = r.voteWeight;
        auto it = taskModels.find(r.role);
        const std::string model = it != taskModels.end() ? it->second : defaultModel;
        j["model"] = model;
        auto p = personas.find(r.role);
        j["persona"] = p != personas.end() ? p->second : personaFromModel(model);
        arr.push_back(std::move(j));
    }
    json::JsonValue v = json::JsonValue::object();
    v["roles"] = std::move(arr);
    v["default_model"] = defaultModel;
    v["task_id"] = taskId;
    return v;
}

json::JsonValue CrewEngine::metaInfo() {
    json::JsonValue f = json::JsonValue::object();
    f["persistence"] = !opt_.persistDir.empty();
    f["resume"] = !opt_.persistDir.empty();
    f["memory"] = opt_.memoryEnabled;
    f["otel"] = opt_.otel != nullptr;
    f["webhook"] = !opt_.webhookUrl.empty();
    f["human_voter"] = true;   // 能力开关（任务级 cfg 决定是否启用）
    f["exec_verify"] = true;
    f["stories"] = true;
    f["debate"] = true;
    f["cancel"] = true;
    f["export"] = true;
    f["write_guard"] = !opt_.approvalToken.empty();
    json::JsonValue v = json::JsonValue::object();
    v["api"] = "v2";
    v["max_concurrent_tasks"] = opt_.maxConcurrentTasks;
    v["features"] = std::move(f);
    json::JsonValue names = json::JsonValue::array();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const RoleDef& r : roles_) names.push_back(r.role);
    }
    v["roles"] = std::move(names);
    return v;
}

uint64_t CrewEngine::subscribe(const std::string& taskId, EventCallback cb) {
    std::lock_guard<std::mutex> lk(mtx_);
    const uint64_t id = subSeq_++;
    subs_[id] = {taskId, std::move(cb)};
    return id;
}

void CrewEngine::unsubscribe(uint64_t subId) {
    std::lock_guard<std::mutex> lk(mtx_);
    subs_.erase(subId);
}

void CrewEngine::setBusy(const std::string&, const std::string&, const std::string&,
                         const std::string&) {
    // 预留（状态注入测试钩子）
}

} // namespace zenith::crew
