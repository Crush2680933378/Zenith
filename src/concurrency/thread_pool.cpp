#include "zenith/concurrency/thread_pool.hpp"

#include <chrono>

namespace zenith::concurrency {

ThreadPool::ThreadPool(size_t numThreads) {
    if (numThreads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        numThreads = hw > 0 ? hw : 4;
    }
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() {
    shutdown();
}

bool ThreadPool::post(Task task) {
    if (!task) return false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_) return false;
        tasks_.emplace_back(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ThreadPool::shutdown() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (stopped_) return;
        stopped_ = true;
    }
    cv_.notify_all();
    for (std::thread& t : workers_) {
        if (t.joinable()) t.join();
    }
}

size_t ThreadPool::pendingTasks() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tasks_.size();
}

bool ThreadPool::waitIdle(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mtx_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!tasks_.empty() || active_.load(std::memory_order_acquire) != 0) {
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) {
            break;
        }
    }
    return tasks_.empty() && active_.load(std::memory_order_acquire) == 0;
}

void ThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopped_ || !tasks_.empty(); });
            if (tasks_.empty()) return;  // stopped_ 且队列已排空 → 退出
            task = std::move(tasks_.front());
            tasks_.pop_front();
            active_.fetch_add(1, std::memory_order_relaxed);
        }
        task();  // 在锁外执行，异常由任务自身负责
        // 只有「池刚刚变空」才需要通知 waitIdle；常见路径仅一次原子减，
        // 不额外加锁（每任务多两次锁会把 500 任务量的用例拖慢到超时）。
        if (active_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            std::lock_guard<std::mutex> lk(mtx_);
            if (tasks_.empty()) cv_.notify_all();
        }
    }
}

}  // namespace zenith::concurrency
