// ============================================================================
//  CronExpr / CronScheduler 实现（M13 第四批）
// ============================================================================
#include "zenith/concurrency/cron.hpp"

#include <chrono>
#include <condition_variable>
#include <cstring>

namespace zenith::concurrency {

namespace {
constexpr int64_t kSecondsPerDay = 86400;

/// 全数字字符串 → int（空串/非数字返回 false）
bool parseNum(const std::string& s, int& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    out = std::atoi(s.c_str());
    return true;
}
} // namespace

bool CronExpr::fieldParse(const std::string& s, int lo, int hi, Field& out) {
    out.lo = lo;
    out.hi = hi;
    out.bits = 0;
    size_t pos = 0;
    auto token = [&]() -> std::string {
        size_t start = pos;
        while (pos < s.size() && s[pos] != ',') ++pos;
        std::string t = s.substr(start, pos - start);
        if (pos < s.size()) ++pos; // 跳过 ','
        return t;
    };
    if (s.empty()) return false;
    for (;;) {
        std::string t = token();
        if (t.empty()) break;
        int step = 1;
        size_t slash = t.find('/');
        if (slash != std::string::npos) {
            if (!parseNum(t.substr(slash + 1), step) || step <= 0) return false;
            t = t.substr(0, slash);
        }
        int a = lo, b = hi;
        if (t == "*") {
            // 全域
        } else {
            size_t dash = t.find('-');
            if (dash != std::string::npos) {
                if (!parseNum(t.substr(0, dash), a) ||
                    !parseNum(t.substr(dash + 1), b)) {
                    return false;
                }
            } else {
                if (!parseNum(t, a)) return false;
                b = a;
                if (step != 1) b = hi; // "N/step" 形态：N 起到域顶按步长
            }
        }
        if (a < lo || b > hi || a > b) return false;
        for (int v = a; v <= b; v += step) out.bits |= 1ULL << v;
        if (pos >= s.size()) break;
    }
    return out.bits != 0;
}

core::Result<CronExpr> CronExpr::parse(const std::string& expr) {
    // 空白切 5 字段
    std::vector<std::string> f;
    std::string cur;
    for (char c : expr) {
        if (c == ' ' || c == '\t') {
            if (!cur.empty()) f.push_back(cur), cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) f.push_back(cur);
    if (f.size() != 5) {
        return core::Result<CronExpr>::Err(core::Error(
            400, "cron expr needs 5 fields (min hour dom month dow): " + expr));
    }
    CronExpr e;
    const char* names[5] = {"minute", "hour", "day-of-month", "month", "day-of-week"};
    Field* fields[5] = {&e.minute_, &e.hour_, &e.dom_, &e.month_, &e.dow_};
    const int lo[5] = {0, 0, 1, 1, 0};
    const int hi[5] = {59, 23, 31, 12, 6};
    for (int i = 0; i < 5; ++i) {
        std::string s = f[static_cast<size_t>(i)];
        // 周字段 7 规范化为 0（0 与 7 都是周日）
        if (i == 4 && s.find('7') != std::string::npos) {
            for (char& c : s) {
                if (c == '7') c = '0';
            }
        }
        if (!fieldParse(s, lo[i], hi[i], *fields[static_cast<size_t>(i)])) {
            return core::Result<CronExpr>::Err(
                core::Error(400, std::string("bad cron field ") + names[i] + ": " +
                                     f[static_cast<size_t>(i)]));
        }
    }
    e.text_ = expr;
    return core::Result<CronExpr>::Ok(std::move(e));
}

int64_t CronExpr::nextAfter(int64_t fromEpochSec) const {
    // 逐日扫描（最长扫 4 年+1 天覆盖 2/29）；本地时区（与系统 cron 一致）
    const int64_t startDay = fromEpochSec / kSecondsPerDay;
    for (int64_t day = startDay; day < startDay + 1462; ++day) {
        const int64_t dayStart = day * kSecondsPerDay;
        time_t t = static_cast<time_t>(dayStart);
        struct tm lt{};
#if defined(_WIN32)
        if (localtime_s(&lt, &t) != 0) return -1;
#else
        if (localtime_r(&t, &lt) == nullptr) return -1;
#endif
        const int month = lt.tm_mon + 1;
        const int dom = lt.tm_mday;
        const int dow = lt.tm_wday;
        if (!match(month_, month)) continue;
        // 日/周语义（Vixie cron）：两字段同时受限 → 任一匹配即触发
        const bool domRestricted = dom_.bits != 0xFFFFFFFEULL; // bits 1..31 全置位
        const bool dowRestricted = dow_.bits != 0x7FULL;       // bits 0..6 全置位
        const bool dayOk = (domRestricted && dowRestricted)
                               ? (match(dom_, dom) || match(dow_, dow))
                               : (match(dom_, dom) && match(dow_, dow));
        if (!dayOk) continue;

        for (int h = 0; h <= 23; ++h) {
            if (!match(hour_, h)) continue;
            for (int m = 0; m <= 59; ++m) {
                if (!match(minute_, m)) continue;
                const int64_t cand = dayStart + h * 3600 + m * 60;
                if (cand <= fromEpochSec) continue;
                return cand;
            }
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// CronScheduler
// ---------------------------------------------------------------------------
uint64_t CronScheduler::add(const std::string& cronExpr, Job job) {
    auto e = CronExpr::parse(cronExpr);
    if (!e) return 0;
    const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count();
    Entry entry{nextId_.fetch_add(1), std::move(e).value(), 0, std::move(job)};
    entry.nextFire = entry.expr.nextAfter(now);
    const uint64_t id = entry.id;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        jobs_.push_back(std::move(entry));
    }
    if (!running_.exchange(true)) thread_ = std::thread([this] { run(); });
    cv_.notify_all();
    return id;
}

bool CronScheduler::remove(uint64_t jobId) {
    std::lock_guard<std::mutex> lk(mtx_);
    for (size_t i = 0; i < jobs_.size(); ++i) {
        if (jobs_[i].id == jobId) {
            jobs_.erase(jobs_.begin() + static_cast<long>(i));
            cv_.notify_all();
            return true;
        }
    }
    return false;
}

size_t CronScheduler::jobCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return jobs_.size();
}

void CronScheduler::run() {
    std::unique_lock<std::mutex> lk(mtx_);
    for (;;) {
        if (!running_.load()) return;
        int64_t soonest = -1;
        for (const Entry& e : jobs_) {
            if (e.nextFire > 0 && (soonest < 0 || e.nextFire < soonest)) soonest = e.nextFire;
        }
        int64_t nowSec = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        if (soonest < 0 || soonest > nowSec) {
            const int64_t waitMs =
                soonest < 0 ? 500 : (soonest - nowSec) * 1000 + 50;
            cv_.wait_for(lk, std::chrono::milliseconds(waitMs));
            continue;
        }
        // 触发所有到点的任务（skip 策略：先推进 nextFire 再执行；job 内可
        // 安全 add/remove——vector 重alloc 不影响已拷贝出的副本）
        std::vector<std::pair<uint64_t, Job>> due;
        for (Entry& e : jobs_) {
            if (e.nextFire > 0 && e.nextFire <= nowSec) {
                e.nextFire = e.expr.nextAfter(nowSec);
                due.emplace_back(e.id, e.job);
            }
        }
        for (auto& [id, job] : due) {
            (void)id;
            lk.unlock();
            ++triggered_;
            job(nowSec);
            lk.lock();
            if (!running_.load()) return;
        }
    }
}

void CronScheduler::stop() {
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

} // namespace zenith::concurrency
