#include "zenith/net/event_loop.hpp"

#include <algorithm>
#include <cstring>

#include "net_detail.hpp"

namespace zenith::net {

namespace {
constexpr size_t kWakeupBuf = 64;
constexpr int64_t kUsPerMs = 1000;

int64_t nowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
} // namespace

EventLoop::EventLoop() : poller_(createPoller()), pollerName_(poller_->name()) {
    threadId_ = std::this_thread::get_id();
    wakeFd_ = detail::createWakeupFd();
    if (wakeFd_ != kInvalidFd) {
        detail::setFdNonBlocking(wakeFd_, true);
        poller_->registerFd(wakeFd_, kPollReadable);
        fds_[wakeFd_] = FdEntry{kPollReadable, [this](uint16_t) { handleWakeupRead(); }};
    }
}

EventLoop::~EventLoop() {
    if (wakeFd_ != kInvalidFd) {
        detail::closeRaw(wakeFd_);
        wakeFd_ = kInvalidFd;
    }
}

void EventLoop::loop() {
    if (looping_) return; // 单线程单次
    threadId_ = std::this_thread::get_id();
    looping_ = true;
    quit_.store(false, std::memory_order_release);
    while (!quit_.load(std::memory_order_acquire)) {
        pollOnce(nextTimeoutMs());
    }
    looping_ = false;
}

void EventLoop::stop() {
    quit_.store(true, std::memory_order_release);
    queueInLoop([] {});
}

void EventLoop::pollOnce(int timeoutMs) {
    pollResults_.clear();
    const int n = poller_->poll(pollResults_, timeoutMs);
    if (n > 0) {
        // 回调可能注销 fd / 修改注册表：先拷贝结果，逐项查表再调用
        for (const PollResult& pr : pollResults_) {
            auto it = fds_.find(pr.fd);
            if (it == fds_.end()) continue;
            FdCallback cb = it->second.cb;
            if (cb) cb(pr.revents);
        }
    }
    runPendingTasks();
    runExpiredTimers();
}

void EventLoop::registerFd(fd_t fd, uint16_t events, FdCallback cb) {
    if (fd == kInvalidFd) return;
    fds_[fd] = FdEntry{events, std::move(cb)};
    poller_->registerFd(fd, events);
}

void EventLoop::updateFd(fd_t fd, uint16_t events) {
    auto it = fds_.find(fd);
    if (it == fds_.end()) return;
    it->second.events = events;
    poller_->updateFd(fd, events);
}

void EventLoop::deregisterFd(fd_t fd) {
    if (fds_.erase(fd) > 0) poller_->deregisterFd(fd);
}

void EventLoop::runInLoop(Task task) {
    if (!task) return;
    if (isInLoopThread()) {
        task();
    } else {
        queueInLoop(std::move(task));
    }
}

void EventLoop::queueInLoop(Task task) {
    if (!task) return;
    {
        std::lock_guard<std::mutex> lk(taskMtx_);
        pendingTasks_.push_back(std::move(task));
    }
    if (!isInLoopThread()) wakeup();
}

uint64_t EventLoop::scheduleAfter(std::chrono::milliseconds delay, Task task) {
    if (!task) return 0;
    const int64_t when = nowUs() + static_cast<int64_t>(delay.count()) * kUsPerMs;
    const uint64_t seq = nextTimerSeq_++;
    {
        std::lock_guard<std::mutex> lk(taskMtx_); // schedule 允许跨线程
        pendingTasks_.push_back([this, when, seq, t = std::move(task)]() mutable {
            timers_.push_back(TimerEntry{when, seq, std::move(t)});
            std::push_heap(timers_.begin(), timers_.end());
        });
    }
    if (!isInLoopThread()) wakeup();
    return seq;
}

bool EventLoop::cancel(uint64_t timerId) {
    if (timerId == 0) return false;
    {
        std::lock_guard<std::mutex> lk(taskMtx_);
        pendingTasks_.push_back([this, timerId] { canceledTimers_.insert(timerId); });
    }
    if (!isInLoopThread()) wakeup();
    return true;
}

void EventLoop::runPendingTasks() {
    std::deque<Task> local;
    {
        std::lock_guard<std::mutex> lk(taskMtx_);
        local.swap(pendingTasks_);
    }
    for (Task& t : local) t();
}

void EventLoop::runExpiredTimers() {
    std::vector<Task> due;
    while (!timers_.empty() && timers_.front().whenUs <= nowUs()) {
        std::pop_heap(timers_.begin(), timers_.end());
        TimerEntry e = std::move(timers_.back());
        timers_.pop_back();
        if (canceledTimers_.erase(e.seq) > 0) continue; // 已取消（墓碑）
        due.push_back(std::move(e.task));
    }
    for (Task& t : due) t(); // 任务内可再 schedule 新定时器
}

int EventLoop::nextTimeoutMs() const {
    {
        // pendingTasks_ 可能被其它线程在锁下并发 push，读也必须持锁
        std::lock_guard<std::mutex> lk(taskMtx_);
        if (!pendingTasks_.empty()) return 0;
    }
    if (timers_.empty()) return 1000; // 兜底上限：stop 等状态仍可及时感知
    const int64_t diffUs = timers_.front().whenUs - nowUs();
    if (diffUs <= 0) return 0;
    return static_cast<int>(diffUs / kUsPerMs + 1);
}

void EventLoop::wakeup() {
    if (wakeFd_ == kInvalidFd) return;
    const char c = 'w';
    detail::rawWrite(wakeFd_, &c, 1);
}

void EventLoop::handleWakeupRead() {
    char buf[kWakeupBuf];
    while (detail::rawRead(wakeFd_, buf, sizeof(buf)) > 0) {
    }
}

} // namespace zenith::net
