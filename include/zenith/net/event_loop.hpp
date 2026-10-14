// ============================================================================
//  zenith::net::EventLoop —— Reactor 事件循环（one loop per thread 模型）
//  - fd 就绪回调（registerFd/updateFd/deregisterFd，仅 loop 线程调用）
//  - 定时器（min-heap + 取消墓碑）
//  - 跨线程任务（runInLoop/queueInLoop，自连接 UDP socket 唤醒）
// ============================================================================
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/net/poller.hpp"
#include "zenith/net/socket.hpp"

namespace zenith::net {

class ZENITH_API EventLoop : private core::NonCopyable {
public:
    using Task = std::function<void()>;

    EventLoop();
    ~EventLoop();

    /// 阻塞循环，直至 stop()。可在任意线程调用一次。
    void loop();

    /// 线程安全；唤醒循环使其退出。
    void stop();

    [[nodiscard]] bool isInLoopThread() const noexcept {
        return std::this_thread::get_id() == threadId_;
    }
    [[nodiscard]] std::thread::id threadId() const noexcept { return threadId_; }
    [[nodiscard]] const char* pollerName() const noexcept { return pollerName_; }

    /* ---- fd 注册（仅 loop 线程） ---- */
    using FdCallback = std::function<void(uint16_t revents)>;
    void registerFd(fd_t fd, uint16_t events, FdCallback cb);
    void updateFd(fd_t fd, uint16_t events); // 保留回调
    void deregisterFd(fd_t fd);

    /* ---- 任务 ---- */
    /// loop 线程直接执行；否则投递并唤醒。
    void runInLoop(Task task);
    /// 总是投递并唤醒。
    void queueInLoop(Task task);

    /* ---- 定时器 ---- */
    /// 返回 timerId（可用于 cancel）。delay <= 0 立即到期。
    uint64_t scheduleAfter(std::chrono::milliseconds delay, Task task);
    bool cancel(uint64_t timerId);

    /// 供测试/工具：处理一轮事件与到期定时器（不阻塞）。
    void pollOnce(int timeoutMs);

private:
    struct FdEntry {
        uint16_t events = kPollNone;
        FdCallback cb;
    };

    void wakeup();
    void handleWakeupRead();
    void runPendingTasks();
    void runExpiredTimers();
    [[nodiscard]] int nextTimeoutMs() const;

    std::unique_ptr<Poller> poller_;
    const char* pollerName_ = "?";

    std::unordered_map<fd_t, FdEntry> fds_; // 仅 loop 线程

    mutable std::mutex taskMtx_; // const 方法（nextTimeoutMs）亦需读取
    std::deque<Task> pendingTasks_;

    struct TimerEntry {
        int64_t whenUs;
        uint64_t seq;
        Task task;
        // 大顶堆语义反转（最早到期优先）
        bool operator<(const TimerEntry& o) const { return whenUs > o.whenUs; }
    };
    std::vector<TimerEntry> timers_;             // 仅 loop 线程
    std::unordered_set<uint64_t> canceledTimers_; // 仅 loop 线程
    uint64_t nextTimerSeq_ = 1;

    fd_t wakeFd_ = kInvalidFd; // 自连接 UDP：跨线程唤醒
    std::atomic<bool> quit_{false};
    std::thread::id threadId_{};
    std::vector<PollResult> pollResults_;
    bool looping_ = false;
};

} // namespace zenith::net
