// kqueue 后端（macOS / BSD）。
#include "zenith/net/poller.hpp"

#include "zenith/core/platform.hpp"

#if defined(ZENITH_PLATFORM_MACOS)

  #include <errno.h>
  #include <sys/event.h>
  #include <unistd.h>

namespace zenith::net {

class KqueuePoller final : public Poller {
public:
    KqueuePoller() : kq_(::kqueue()) {}

    ~KqueuePoller() override {
        if (kq_ >= 0) ::close(kq_);
    }

    bool registerFd(fd_t fd, uint16_t events) override {
        if (kq_ < 0 || fd == kInvalidFd) return false;
        if ((events & kPollReadable) != 0 && !keventAdd(fd, EVFILT_READ, EV_ADD)) return false;
        if ((events & kPollWritable) != 0 && !keventAdd(fd, EVFILT_WRITE, EV_ADD)) return false;
        if ((events & kPollReadable) == 0) keventAdd(fd, EVFILT_READ, EV_DELETE);
        if ((events & kPollWritable) == 0) keventAdd(fd, EVFILT_WRITE, EV_DELETE);
        return true;
    }

    bool updateFd(fd_t fd, uint16_t events) override { return registerFd(fd, events); }

    bool deregisterFd(fd_t fd) override {
        if (kq_ < 0) return false;
        keventAdd(fd, EVFILT_READ, EV_DELETE);
        keventAdd(fd, EVFILT_WRITE, EV_DELETE);
        return true;
    }

    int poll(std::vector<PollResult>& out, int timeoutMs) override {
        if (kq_ < 0) return -1;
        struct timespec ts{}, *tsp = nullptr;
        if (timeoutMs >= 0) {
            ts.tv_sec = timeoutMs / 1000;
            ts.tv_nsec = (timeoutMs % 1000) * 1000000L;
            tsp = &ts;
        }
        events_.resize(kMaxEvents);
        const int n = ::kevent(kq_, nullptr, 0, events_.data(),
                               static_cast<int>(events_.size()), tsp);
        if (n <= 0) return n == 0 ? 0 : -1;
        for (int i = 0; i < n; ++i) {
            const struct kevent& ke = events_[i];
            uint16_t re = kPollNone;
            if (ke.filter == EVFILT_READ) re |= kPollReadable;
            if (ke.filter == EVFILT_WRITE) re |= kPollWritable;
            if ((ke.flags & EV_ERROR) != 0) re |= kPollError;
            if ((ke.flags & EV_EOF) != 0) re |= kPollHangup;
            out.push_back(PollResult{static_cast<fd_t>(ke.ident), re});
        }
        return n;
    }

    [[nodiscard]] const char* name() const override { return "kqueue"; }

private:
    static constexpr int kMaxEvents = 256;

    bool keventAdd(fd_t fd, int16_t filter, uint16_t flags) {
        struct kevent ke{};
        EV_SET(&ke, static_cast<uintptr_t>(fd), filter, flags, 0, 0, nullptr);
        return ::kevent(kq_, &ke, 1, nullptr, 0, nullptr) == 0;
    }

    int kq_;
    std::vector<struct kevent> events_;
};

std::unique_ptr<Poller> createKqueuePoller() { return std::make_unique<KqueuePoller>(); }

} // namespace zenith::net

#endif // ZENITH_PLATFORM_MACOS
