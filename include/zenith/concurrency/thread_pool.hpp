// ============================================================================
//  ThreadPool —— 固定线程数任务池（M1 基础版）
//  - post() 投递任意可调用对象；shutdown() 后 post 返回 false
//  - 析构自动 shutdown()：停止接受新任务，把已排队任务全部执行完后 join
//  - M2 接入 EventLoop 后扩展 work-stealing 与优先级（接口保持兼容）
// ============================================================================
#pragma once

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"

namespace zenith::concurrency {

class ZENITH_API ThreadPool : private core::NonCopyable {
public:
    using Task = std::function<void()>;

    /// numThreads == 0 时自动取 hardware_concurrency()。
    explicit ThreadPool(size_t numThreads = 0);
    ~ThreadPool();

    /// 投递任务；线程池已关闭时返回 false 且不执行。
    bool post(Task task);

    /// 幂等。唤醒所有工作线程，排空队列后 join。
    void shutdown();

    [[nodiscard]] size_t threadCount() const noexcept { return workers_.size(); }
    [[nodiscard]] size_t pendingTasks() const;

private:
    void workerLoop();

    std::vector<std::thread> workers_;
    std::deque<Task>         tasks_;
    mutable std::mutex       mtx_;
    std::condition_variable  cv_;
    bool                     stopped_ = false;
};

} // namespace zenith::concurrency
