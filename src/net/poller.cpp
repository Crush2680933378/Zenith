#include "zenith/net/poller.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <map>
#include <thread>

#include "zenith/core/platform.hpp"

#if defined(ZENITH_PLATFORM_WINDOWS)
  #ifndef _WIN32_WINNT
    #define _WIN32_WINNT 0x0601
  #endif
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
#else
  #include <sys/select.h>
#endif

namespace zenith::net {

// 平台后端可用性 ------------------------------------------------------------
#if defined(ZENITH_PLATFORM_LINUX)
std::unique_ptr<Poller> createEpollPoller();
#elif defined(ZENITH_PLATFORM_MACOS)
std::unique_ptr<Poller> createKqueuePoller();
#elif defined(ZENITH_PLATFORM_WINDOWS)
std::unique_ptr<Poller> createWsaPollPoller();
#endif

std::unique_ptr<Poller> createSelectPoller();

std::unique_ptr<Poller> createPoller() {
#if defined(ZENITH_PLATFORM_LINUX)
    return createEpollPoller();
#elif defined(ZENITH_PLATFORM_MACOS)
    return createKqueuePoller();
#elif defined(ZENITH_PLATFORM_WINDOWS)
    return createWsaPollPoller();
#else
    return createSelectPoller();
#endif
}

// ---------------------------------------------------------------------------
// select 后端：全平台兜底。
// 注意：Windows fd_set 受 FD_SETSIZE 限制（此处提升到 4096）；
//       生产环境 Linux/macOS/Windows 分别走 epoll/kqueue/WSAPoll。
// ---------------------------------------------------------------------------
#if defined(ZENITH_PLATFORM_WINDOWS)
  #if !defined(ZENITH_POLLER_CUSTOM_FD_SETSIZE)
    #undef FD_SETSIZE
    #define FD_SETSIZE 4096
  #endif
#endif

class SelectPoller final : public Poller {
public:
    bool registerFd(fd_t fd, uint16_t events) override {
        if (fd == kInvalidFd) return false;
        const auto [it, inserted] = interested_.emplace(fd, events);
        if (!inserted) it->second = events;
        return true;
    }

    bool updateFd(fd_t fd, uint16_t events) override { return registerFd(fd, events); }

    bool deregisterFd(fd_t fd) override { return interested_.erase(fd) > 0; }

    int poll(std::vector<PollResult>& out, int timeoutMs) override {
        fd_set rset, wset;
        FD_ZERO(&rset);
        FD_ZERO(&wset);

        int maxfd = -1;
        bool wantRead = false, wantWrite = false;
        for (const auto& [fd, ev] : interested_) {
            const int f = static_cast<int>(fd);
            if ((ev & kPollReadable) != 0) {
                FD_SET(f, &rset);
                wantRead = true;
            }
            if ((ev & kPollWritable) != 0) {
                FD_SET(f, &wset);
                wantWrite = true;
            }
            if (f > maxfd) maxfd = f;
        }
        if (!wantRead && !wantWrite) {
            // 无关注 fd：纯定时休眠
            if (timeoutMs < 0) timeoutMs = 100; // 防永久休死
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
            return 0;
        }

        timeval tv{};
        timeval* tvp = nullptr;
        if (timeoutMs >= 0) {
            tv.tv_sec = timeoutMs / 1000;
            tv.tv_usec = (timeoutMs % 1000) * 1000;
            tvp = &tv;
        }

        const int n = ::select(maxfd + 1, &rset, &wset, nullptr, tvp);
        if (n <= 0) return n == 0 ? 0 : -1;

        int fired = 0;
        for (const auto& [fd, ev] : interested_) {
            const int f = static_cast<int>(fd);
            uint16_t re = kPollNone;
            if (FD_ISSET(f, &rset)) re |= kPollReadable;
            if (FD_ISSET(f, &wset)) re |= kPollWritable;
            if (re != kPollNone) {
                out.push_back(PollResult{fd, re});
                ++fired;
                if (fired >= n) break;
            }
        }
        return fired;
    }

    [[nodiscard]] const char* name() const override { return "select"; }

private:
    std::map<fd_t, uint16_t> interested_;
};

std::unique_ptr<Poller> createSelectPoller() { return std::make_unique<SelectPoller>(); }

} // namespace zenith::net
