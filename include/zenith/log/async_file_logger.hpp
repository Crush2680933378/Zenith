// ============================================================================
//  AsyncFileLogger —— 异步滚动文件日志 sink
//  - 写路径：格式化（一次）→ 无锁 MPSC 队列 → 后台线程落盘
//  - 滚动策略：单文件超过 rollBytes 时轮转，保留 keepFiles 个历史文件
//    （base.log → base.log.1 → ... → base.log.N，N 最旧）
//  - 队列满时丢弃最旧策略会导致乱序，故选择丢弃新日志并计数（droppedCount）
//  - 析构保证：停止 → 唤醒 → 排空队列 → 关闭文件（不丢已接受日志）
// ============================================================================
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "zenith/concurrency/mpsc_queue.hpp"
#include "zenith/core/export.hpp"
#include "zenith/log/logger.hpp"

namespace zenith::log {

class ZENITH_API AsyncFileLogger : public LogSink {
public:
    /// basePath：日志文件完整路径（如 logs/zenith.log），目录需已存在。
    AsyncFileLogger(std::string basePath, size_t rollBytes = 10 * 1024 * 1024,
                    int keepFiles = 5);
    ~AsyncFileLogger() override;

    AsyncFileLogger(const AsyncFileLogger&) = delete;
    AsyncFileLogger& operator=(const AsyncFileLogger&) = delete;

    void write(const LogRecord& record) override;
    void flush() override; // 唤醒后台线程立即落盘

    /// 因队列满而被丢弃的日志条数（监控指标）。
    [[nodiscard]] uint64_t droppedCount() const noexcept { return dropped_.load(); }

private:
    void workerLoop();
    bool openFile();
    void closeFile();
    void rollFiles();
    void drainBatch();

    std::string path_;
    size_t      rollBytes_;
    int         keepFiles_;

    // 队列堆上持有：65536 槽约 2.6MB，避免 AsyncFileLogger 栈构造时爆栈
    std::unique_ptr<concurrency::MpscRingQueue<std::string, 65536>> queue_ =
        std::make_unique<concurrency::MpscRingQueue<std::string, 65536>>();

    std::thread             worker_;
    std::mutex              wakeMtx_;
    std::condition_variable wakeCv_;
    std::atomic<bool>       stop_{false};

    FILE*     fp_      = nullptr;
    size_t    written_ = 0;
    std::atomic<uint64_t> dropped_{0};
};

} // namespace zenith::log
