// ============================================================================
//  zenith::net::EventLoopThreadPool —— one loop per thread 的 IO 线程组
//  accept 在主 loop；新连接 round-robin 分配到 IO loop。
// ============================================================================
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/net/event_loop.hpp"

namespace zenith::net {

class ZENITH_API EventLoopThreadPool : private core::NonCopyable {
public:
    /// n == 0 时取 hardware_concurrency()-1（至少 1）。
    explicit EventLoopThreadPool(size_t n = 0, std::string name = "zenith-io");
    ~EventLoopThreadPool();

    void start();
    void stop(); // 停止全部 loop 并 join（幂等）

    /// round-robin 取一个 IO loop；未启动时返回 nullptr。
    EventLoop* nextLoop();

    [[nodiscard]] size_t size() const noexcept { return loops_.size(); }

private:
    std::vector<std::unique_ptr<EventLoop>> loops_;
    std::vector<std::thread> threads_;
    std::atomic<size_t> next_{0};
    std::string name_;
    bool started_ = false;
};

} // namespace zenith::net
