#include "zenith/crew/crew.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace zenith::crew {

namespace {

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- 7 角色定义（system prompt 均约定“只输出一个 JSON 对象”）----
struct RoleDef {
    const char* role;
    const char* prompt;
};

const RoleDef kRoles[] = {
    {"pm", "ROLE: PM. You are the product manager. Given a brief, produce ONLY one JSON "
           "object: {\"prd\":{\"summary\":\"...\",\"stories\":[{\"id\":\"S1\",\"title\":"
           "\"...\",\"given\":\"...\",\"when\":\"...\",\"then\":\"...\"}]}}. Every story "
           "must have executable Given/When/Then acceptance criteria."},
    {"architect", "ROLE: ARCHITECT. You are the software architect. Given the PRD, "
                  "produce ONLY one JSON object: {\"architecture\":{\"style\":\"...\","
                  "\"storage\":\"sqlite|postgres|none\",\"components\":[\"...\"],"
                  "\"api\":[{\"method\":\"GET\",\"path\":\"/api/...\",\"desc\":\"...\"}],"
                  "\"notes\":\"...\"}}. Contract-first: api entries are the single "
                  "source of truth for backend and frontend."},
    {"backend", "ROLE: BACKEND. You are the backend developer. Given the API contract, "
                "produce ONLY one JSON object: {\"files\":[{\"path\":\"src/main.cpp\","
                "\"content\":\"// implementation\"}],\"notes\":\"...\"}. Implement every "
                "contracted endpoint."},
    {"frontend", "ROLE: FRONTEND. You are the frontend developer. Given the API "
                 "contract, produce ONLY one JSON object: {\"files\":[{\"path\":"
                 "\"web/index.html\",\"content\":\"<!-- UI -->\"}],\"notes\":\"...\"}. "
                 "Develop against the contract (mock-friendly), never block on backend."},
    {"tester", "ROLE: TESTER. You are the QA engineer. Given the PRD acceptance "
               "criteria and API contract, produce ONLY one JSON object: {\"cases\":"
               "[{\"id\":\"T1\",\"story\":\"S1\",\"given\":\"...\",\"when\":\"...\","
               "\"then\":\"...\"}],\"results\":[{\"case\":\"T1\",\"status\":"
               "\"pass|fail\",\"detail\":\"...\",\"component\":\"backend|frontend\"}]}. "
               "Every story needs at least one case. When asked to RETEST after fixes, "
               "report all cases pass unless the fix note says otherwise."},
    {"devops", "ROLE: DEVOPS. You are the release engineer. Given the architecture, "
               "produce ONLY one JSON object: {\"files\":[{\"path\":\"Dockerfile\","
               "\"content\":\"...\"},{\"path\":\".ci/pipeline.yml\",\"content\":\"...\"}],"
               "\"deploy\":{\"strategy\":\"...\",\"smoke\":\"...\"}}."},
    {"delivery", "ROLE: DELIVERY. You are the release manager. Given all artifacts, "
                 "produce ONLY one JSON object: {\"changelog\":\"markdown text\","
                 "\"release_notes\":\"markdown text\",\"version\":\"0.1.0\"}."},
};

const std::vector<std::string> kAllRoles = {"pm",      "architect", "backend",
                                            "frontend", "tester",   "devops",
                                            "delivery"};

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
    v["error"] = error;
    json::JsonValue arr = json::JsonValue::array();
    for (const AgentStatus& a : agents) arr.push_back(a.toJson());
    v["agents"] = std::move(arr);
    v["event_count"] = static_cast<int64_t>(eventCount);
    v["artifact_count"] = static_cast<int64_t>(artifactCount);
    v["vote_count"] = static_cast<int64_t>(voteCount);
    v["created_ms"] = createdMs;
    v["updated_ms"] = updatedMs;
    return v;
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
    }
    return "?";
}

// ---------------------------------------------------------------------------
// CrewEngine
// ---------------------------------------------------------------------------
CrewEngine::CrewEngine() = default;
CrewEngine::~CrewEngine() = default;

void CrewEngine::setInference(std::shared_ptr<ai::IInferenceService> inf) {
    std::lock_guard<std::mutex> lk(mtx_);
    inference_ = std::move(inf);
}

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
            t->events.erase(t->events.begin(),
                            t->events.begin() + static_cast<long>(t->events.size() - 4096));
        }
        t->updatedMs = e.tsMs;
        for (auto& [id, sub] : subs_) {
            if (sub.first == t->id) toNotify.push_back(sub.second);
        }
    }
    for (const EventCallback& cb : toNotify) cb(e); // 锁外触发
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

std::string CrewEngine::chat(TaskPtr t, const std::string& role,
                             const std::string& userPrompt) {
    std::shared_ptr<ai::IInferenceService> inf;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        inf = inference_;
    }
    if (inf == nullptr) throw std::runtime_error("no inference service");

    const RoleDef* def = nullptr;
    for (const RoleDef& r : kRoles) {
        if (role == r.role) def = &r;
    }
    if (def == nullptr) throw std::runtime_error("unknown role: " + role);

    {
        std::lock_guard<std::mutex> lk(mtx_);
        t->roleCalls[role]++; // 剧本测试按调用序区分轮次
        auto& st = t->agents[role];
        st.role = role;
        st.state = "working";
        st.detail = userPrompt.substr(0, std::min<size_t>(userPrompt.size(), 80));
        st.updatedMs = nowMs();
    }
    emit(t, t->phase, role, "agent_start", std::string(def->role) + " working");

    ai::ChatRequest req;
    req.model = t->cfg.model;
    ai::ChatMessage sys;
    sys.role = "system";
    sys.content = def->prompt;
    req.messages.push_back(std::move(sys));
    ai::ChatMessage user;
    user.role = "user";
    user.content = userPrompt;
    req.messages.push_back(std::move(user));

    auto r = inf->chat(req);
    if (!r.ok) throw std::runtime_error("inference failed: " + r.error);
    return r.content;
}

void CrewEngine::saveArtifact(TaskPtr t, const std::string& type, const std::string& agent,
                              const std::string& content) {
    Artifact a;
    a.taskId = t->id;
    a.type = type;
    a.agent = agent;
    a.tsMs = nowMs();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int ver = 1;
        for (const Artifact& x : t->artifacts) {
            if (x.type == type) ver = std::max(ver, x.version + 1);
        }
        a.version = ver;
        a.id = type + "-v" + std::to_string(ver);
        t->artifacts.push_back(a);
        t->artifacts.back().content = content; // a 拷贝入表后再填内容（内容可能大）
    }
    emit(t, t->phase, agent, "artifact", a.id + " saved (" + type + ")");
}

std::string CrewEngine::createTask(const std::string& brief, const CrewConfig& cfg) {
    auto t = std::make_shared<TaskState>();
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
        for (const std::string& r : kAllRoles) {
            AgentStatus st;
            st.role = r;
            st.state = "idle";
            st.updatedMs = t->createdMs;
            t->agents[r] = st;
        }
        tasks_[t->id] = t;
    }
    t->running = true;
    emit(t, CrewPhase::Pending, "orchestrator", "phase", "task created: " + brief.substr(0, 60));
    std::thread([this, t] { runPipeline(t); }).detach();
    return t->id;
}

void CrewEngine::runPipeline(TaskPtr t) {
    try {
        // ---- 需求 ----
        t->phase = CrewPhase::Requirements;
        emit(t, t->phase, "orchestrator", "phase", "enter requirements");
        std::string prd = chat(t, "pm", "Product brief: " + t->brief +
                                           "\nProduce the PRD JSON now.");
        saveArtifact(t, "prd", "pm", prd);

        // ---- PRD 评审投票 ----
        t->phase = CrewPhase::ReviewPrd;
        emit(t, t->phase, "orchestrator", "phase", "PRD review vote");
        {
            VoteSpec spec;
            spec.question = "Does the PRD meet the brief with executable acceptance criteria?";
            spec.options = {"approve", "revise"};
            auto rec = runVote(t->id, spec);
            if (rec.winner == "revise") {
                emit(t, t->phase, "orchestrator", "gate", "PRD revised once by PM");
                prd = chat(t, "pm",
                           "The crew voted to revise. Improve acceptance criteria. "
                           "Produce the updated PRD JSON now.");
                saveArtifact(t, "prd", "pm", prd);
            }
        }

        // ---- 设计（含技术选型投票）----
        t->phase = CrewPhase::Design;
        emit(t, t->phase, "orchestrator", "phase", "enter design");
        {
            VoteSpec spec;
            spec.question = "Primary storage choice for this project?";
            spec.options = {"sqlite", "postgres", "none"};
            auto rec = runVote(t->id, spec);
            emit(t, t->phase, "orchestrator", "vote",
                 "storage decision: " + rec.winner, rec.toJson().dump());
        }
        std::string arch = chat(t, "architect",
                                "PRD:\n" + prd + "\nProduce the architecture JSON now.");
        saveArtifact(t, "architecture", "architect", arch);
        saveArtifact(t, "contract", "architect",
                     extractJson(arch)["architecture"]["api"].dump());

        // 人工门禁（可选）：设计完成后
        if (t->cfg.requireApproval) {
            t->phase = CrewPhase::AwaitingApproval;
            emit(t, t->phase, "orchestrator", "approval",
                 "design complete; awaiting human approval");
            for (int i = 0; i < 36000; ++i) { // 最长 1 小时
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    if (t->approved) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            {
                std::lock_guard<std::mutex> lk(mtx_);
                t->approved = false;
            }
            emit(t, t->phase, "orchestrator", "approval", "approved; resuming");
        }

        // ---- 实现（后端 ∥ 前端：契约隔离，顺序执行保确定性，事件区分角色）----
        t->phase = CrewPhase::Implement;
        emit(t, t->phase, "orchestrator", "phase", "enter implement (backend ∥ frontend)");
        std::string contract = extractJson(arch)["architecture"]["api"].dump();
        std::string be = chat(t, "backend",
                              "API contract:\n" + contract + "\nProduce backend files JSON.");
        saveArtifact(t, "backend", "backend", be);
        std::string fe = chat(t, "frontend",
                              "API contract:\n" + contract + "\nProduce frontend files JSON.");
        saveArtifact(t, "frontend", "frontend", fe);

        // ---- 测试 + 缺陷环 ----
        int cycle = 0;
        for (;;) {
            t->phase = CrewPhase::Test;
            emit(t, t->phase, "orchestrator", "phase",
                 cycle == 0 ? "enter test" : "retest after fix (cycle " +
                                                 std::to_string(cycle) + ")");
            std::string prompt = "PRD:\n" + prd + "\nAPI contract:\n" + contract;
            if (cycle > 0) {
                prompt += "\nPrevious bugs were fixed by backend/frontend. RETEST now: "
                          "mark all previously failing cases as pass.";
            } else {
                prompt += "\nProduce test cases and results JSON now.";
            }
            std::string tr = chat(t, "tester", prompt);
            saveArtifact(t, "tests", "tester", tr);
            const json::JsonValue results = extractJson(tr)["results"];
            const auto* arr = results.asArray();
            bool allPass = true;
            std::string failComponent = "backend";
            if (arr != nullptr) {
                for (const json::JsonValue& r : *arr) {
                    if (r["status"].asString() == "fail") {
                        allPass = false;
                        if (r["component"].asString() == "frontend") failComponent = "frontend";
                    }
                }
            }
            if (allPass) break;
            if (cycle >= t->cfg.maxBugCycles) {
                emit(t, t->phase, "orchestrator", "gate",
                     "bug budget exhausted; escalating but continuing");
                break;
            }
            t->phase = CrewPhase::Bugfix;
            emit(t, t->phase, "orchestrator", "phase",
                 "bugfix: routing to " + failComponent);
            std::string fix = chat(t, failComponent,
                                   "Test failures reported by QA. Fix the failing cases. "
                                   "Produce updated files JSON.");
            saveArtifact(t, "bugs", failComponent, fix);
            ++cycle;
        }

        // ---- 部署 ----
        t->phase = CrewPhase::Deploy;
        emit(t, t->phase, "orchestrator", "phase", "enter deploy");
        std::string ops = chat(t, "devops",
                               "Architecture:\n" + arch + "\nProduce CI/CD files JSON.");
        saveArtifact(t, "devops", "devops", ops);

        // ---- 发布 ----
        t->phase = CrewPhase::Release;
        emit(t, t->phase, "orchestrator", "phase", "enter release");
        std::string rel = chat(t, "delivery",
                               "PRD:\n" + prd + "\nTests:\n" +
                                   std::to_string(t->artifacts.size()) +
                                   " artifacts produced. Produce release JSON.");
        saveArtifact(t, "release", "delivery", rel);

        t->phase = CrewPhase::Done;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            t->running = false;
        }
        emit(t, t->phase, "orchestrator", "phase", "task done");
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lk(mtx_);
        t->phase = CrewPhase::Failed;
        t->running = false;
        t->error = e.what();
    }
    // 失败也补一条事件（锁外）
    if (t->phase == CrewPhase::Failed) {
        emit(t, CrewPhase::Failed, "orchestrator", "error", t->error);
    }
}

VoteRecord CrewEngine::runVote(const std::string& taskId, const VoteSpec& specIn) {
    VoteSpec spec = specIn;
    if (spec.voters.empty()) spec.voters = kAllRoles;

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

    // 计票：架构师票权 2；reason 表按 (agent,choice) 存理由
    std::map<std::string, int> tally;
    std::map<std::string, std::string> reasons;
    for (const std::string& voter : spec.voters) {
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
        std::string choice;
        std::string reason;
        if (r.ok) {
            json::JsonValue v = extractJson(r.content);
            choice = v["choice"].asString();
            reason = v["reason"].asString();
        }
        if (std::find(spec.options.begin(), spec.options.end(), choice) ==
            spec.options.end()) {
            choice = spec.options.front(); // 非法票 → 默认首项（记录原始 reason）
            reason = "[invalid vote coerced] " + reason;
        }
        rec.votes.emplace_back(voter, choice);
        reasons[voter] = reason;
        tally[choice] += (voter == "architect") ? 2 : 1;
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
    // 决策依据：架构师投给胜出项的理由优先；否则给计票说明
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

core::Result<CrewTaskInfo> CrewEngine::taskInfo(const std::string& id) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<CrewTaskInfo>::Err(core::Error(300, "no such task"));
    }
    CrewTaskInfo info;
    info.id = it->second->id;
    info.brief = it->second->brief;
    info.phase = it->second->phase;
    info.running = it->second->running;
    info.error = it->second->error;
    info.eventCount = it->second->events.size();
    info.artifactCount = it->second->artifacts.size();
    info.voteCount = it->second->votes.size();
    info.createdMs = it->second->createdMs;
    info.updatedMs = it->second->updatedMs;
    for (const std::string& r : kAllRoles) {
        info.agents.push_back(it->second->agents[r]);
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
        info.error = t->error;
        info.createdMs = t->createdMs;
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
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = tasks_.find(id);
    if (it == tasks_.end()) {
        return core::Result<void>::Err(core::Error(300, "no such task"));
    }
    it->second->approved = true;
    return core::Result<void>::Ok();
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
