#include "zenith/net/event_loop_thread_pool.hpp"

namespace zenith::net {

EventLoopThreadPool::EventLoopThreadPool(size_t n, std::string name)
    : name_(std::move(name)) {
    if (n == 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        n = hw > 1 ? hw - 1 : 1;
    }
    loops_.reserve(n);
    for (size_t i = 0; i < n; ++i) loops_.emplace_back(new EventLoop());
}

EventLoopThreadPool::~EventLoopThreadPool() { stop(); }

void EventLoopThreadPool::start() {
    if (started_) return;
    started_ = true;
    const std::string base = name_.empty() ? std::string("zenith-io") : name_;
    for (size_t i = 0; i < loops_.size(); ++i) {
        EventLoop* loop = loops_[i].get();
        threads_.emplace_back([loop, &base, i] {
            // 线程名设置留待各平台专属 API（嵌入式场景按需补充）
            loop->loop();
        });
    }
}

void EventLoopThreadPool::stop() {
    if (!started_) return;
    for (auto& l : loops_) l->stop();
    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();
    started_ = false;
}

EventLoop* EventLoopThreadPool::nextLoop() {
    if (loops_.empty()) return nullptr;
    const size_t i = next_.fetch_add(1, std::memory_order_relaxed) % loops_.size();
    return loops_[i].get();
}

} // namespace zenith::net
