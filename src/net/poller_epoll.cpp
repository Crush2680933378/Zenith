// epoll 后端（Linux）。WSAPoll/kqueue 头文件隔离在各自翻译单元。
#include "zenith/net/poller.hpp"

#include "zenith/core/platform.hpp"

#if defined(ZENITH_PLATFORM_LINUX)

  #include <errno.h>
  #include <map>
  #include <sys/epoll.h>
  #include <unistd.h>

namespace zenith::net {

namespace {
uint32_t toNativeEvents(uint16_t events) {
    uint32_t n = 0;
    if ((events & kPollReadable) != 0) n |= EPOLLIN;
    if ((events & kPollWritable) != 0) n |= EPOLLOUT;
    return n;
}
uint16_t fromNativeEvents(uint32_t revents) {
    uint16_t r = kPollNone;
    if ((revents & EPOLLIN) != 0) r |= kPollReadable;
    if ((revents & EPOLLOUT) != 0) r |= kPollWritable;
    if ((revents & EPOLLERR) != 0) r |= kPollError;
    if ((revents & EPOLLHUP) != 0) r |= kPollHangup;
    return r;
}
} // namespace

class EpollPoller final : public Poller {
public:
    EpollPoller() : epfd_(::epoll_create1(EPOLL_CLOEXEC)) {}

    ~EpollPoller() override {
        if (epfd_ >= 0) ::close(epfd_);
    }

    bool registerFd(fd_t fd, uint16_t events) override {
        if (epfd_ < 0 || fd == kInvalidFd) return false;
        epoll_event ev{};
        ev.data.fd = static_cast<int>(fd);
        ev.events = toNativeEvents(events);
        if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, static_cast<int>(fd), &ev) == 0) return true;
        if (errno == EEXIST) {
            return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, static_cast<int>(fd), &ev) == 0;
        }
        return false;
    }

    bool updateFd(fd_t fd, uint16_t events) override {
        if (epfd_ < 0) return false;
        epoll_event ev{};
        ev.data.fd = static_cast<int>(fd);
        ev.events = toNativeEvents(events);
        return ::epoll_ctl(epfd_, EPOLL_CTL_MOD, static_cast<int>(fd), &ev) == 0;
    }

    bool deregisterFd(fd_t fd) override {
        if (epfd_ < 0) return false;
        return ::epoll_ctl(epfd_, EPOLL_CTL_DEL, static_cast<int>(fd), nullptr) == 0;
    }

    int poll(std::vector<PollResult>& out, int timeoutMs) override {
        if (epfd_ < 0) return -1;
        events_.resize(kMaxEvents);
        const int n = ::epoll_wait(epfd_, events_.data(),
                                   static_cast<int>(events_.size()), timeoutMs);
        if (n <= 0) return n == 0 ? 0 : -1;
        for (int i = 0; i < n; ++i) {
            out.push_back(PollResult{static_cast<fd_t>(events_[i].data.fd),
                                     fromNativeEvents(events_[i].events)});
        }
        return n;
    }

    [[nodiscard]] const char* name() const override { return "epoll"; }

private:
    static constexpr int kMaxEvents = 256;
    int epfd_;
    std::vector<epoll_event> events_;
};

std::unique_ptr<Poller> createEpollPoller() { return std::make_unique<EpollPoller>(); }

} // namespace zenith::net

#endif // ZENITH_PLATFORM_LINUX
