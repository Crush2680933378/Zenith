// ============================================================================
//  zenith::concurrency::Cron —— cron 定时任务（M13 第四批 / sched_cron）
//
//  自研 5 字段 cron 表达式（分 时 日 月 周；支持 , - * / 与数字）：
//    */5 * * * *     每 5 分钟
//    0 9 * * 1-5     工作日 9 点
//    30 3 1,15 * *   每月 1/15 号 3:30
//  CronScheduler：后台线程按 next-fire 睡眠触发；错过（睡眠超期/卡顿）
//  只补跑一次（skip 策略）；线程安全 add/remove；优雅关闭。
//  时间基准本地时区（与系统 cron 一致）。
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::concurrency {

/// 5 字段 cron 表达式（值域：分 0-59 / 时 0-23 / 日 1-31 / 月 1-12 / 周 0-6）
class ZENITH_API CronExpr {
public:
    /// 解析失败返回 Err（消息含出错字段）
    static core::Result<CronExpr> parse(const std::string& expr);

    /// from 之后（不含 from，精确到秒）的下一次触发时刻（epoch 秒）
    [[nodiscard]] int64_t nextAfter(int64_t fromEpochSec) const;

    [[nodiscard]] std::string text() const { return text_; }

private:
    struct Field {
        uint64_t bits = 0; // 位图：bit i 置位 = 值 i 匹配（周字段 bit7 冗余 7=0）
        int lo = 0, hi = 0;
    };
    static bool fieldParse(const std::string& s, int lo, int hi, Field& out);
    static bool match(const Field& f, int v) { return (f.bits >> v) & 1ULL; }

    Field minute_, hour_, dom_, month_, dow_;
    std::string text_;
};

/// 后台调度器：注册 cron 任务（返回 job id，0=失败），remove 取消
class ZENITH_API CronScheduler {
public:
    using Job = std::function<void(int64_t scheduledSec)>;

    CronScheduler() = default;
    ~CronScheduler() { stop(); }
    CronScheduler(const CronScheduler&) = delete;
    CronScheduler& operator=(const CronScheduler&) = delete;

    /// 注册并启动调度线程（多次调用幂等）。异常表达式返回 0。
    uint64_t add(const std::string& cronExpr, Job job);
    bool remove(uint64_t jobId);
    [[nodiscard]] size_t jobCount() const;
    [[nodiscard]] uint64_t triggerCount() const { return triggered_.load(); }

    void stop();

private:
    struct Entry {
        uint64_t id;
        CronExpr expr;
        int64_t nextFire;
        Job job;
    };
    void run();

    mutable std::mutex mtx_;
    std::vector<Entry> jobs_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> triggered_{0};
    std::atomic<uint64_t> nextId_{1};
    std::thread thread_;
    std::condition_variable cv_; // add/remove 唤醒重算睡眠
};

} // namespace zenith::concurrency
