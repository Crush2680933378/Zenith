#include "zenith/concurrency/thread_pool.hpp"

namespace zenith::concurrency {

ThreadPool::ThreadPool(size_t numThreads) {
    if (numThreads == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        numThreads        = hw > 0 ? hw : 4;
    }
    workers_.reserve(numThreads);
    for (size_t i = 0; i < numThreads; ++i) {
        workers_.emplace_back([this] { workerLoop(); });
    }
}

ThreadPool::~ThreadPool() { shutdown(); }

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

void ThreadPool::workerLoop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [this] { return stopped_ || !tasks_.empty(); });
            if (tasks_.empty()) return; // stopped_ 且队列已排空 → 退出
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }
        task(); // 在锁外执行，异常由任务自身负责
    }
}

} // namespace zenith::concurrency
