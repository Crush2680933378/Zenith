// WSAPoll 后端（Windows Vista+）。
// 已知行为差异：连接断开时报告 POLLRDNORM 而非 POLLHUP —— 与 POSIX poll
// 相同的兜底方式：读到 0 字节即视为对端关闭（TcpConnection 已处理）。
#include "zenith/net/poller.hpp"

#include <map>

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
  #include <ws2tcpip.h>

namespace zenith::net {

namespace {
uint16_t toNativeEvents(uint16_t events) {
    uint16_t n = 0;
    if ((events & kPollReadable) != 0) n |= POLLIN;
    if ((events & kPollWritable) != 0) n |= POLLOUT;
    return n;
}
uint16_t fromNativeEvents(uint16_t revents) {
    uint16_t r = kPollNone;
    if ((revents & POLLIN) != 0) r |= kPollReadable;
    if ((revents & POLLOUT) != 0) r |= kPollWritable;
    if ((revents & (POLLERR | POLLNVAL)) != 0) r |= kPollError;
    if ((revents & POLLHUP) != 0) r |= kPollHangup;
    return r;
}
} // namespace

class WsaPollPoller final : public Poller {
public:
    ~WsaPollPoller() override = default;

    bool registerFd(fd_t fd, uint16_t events) override {
        if (fd == kInvalidFd) return false;
        auto it = index_.find(fd);
        if (it == index_.end()) {
            index_[fd] = fds_.size();
            WSAPOLLFD p{};
            p.fd = static_cast<SOCKET>(fd);
            p.events = toNativeEvents(events);
            p.revents = 0;
            fds_.push_back(p);
        } else {
            fds_[it->second].events = toNativeEvents(events);
        }
        return true;
    }

    bool updateFd(fd_t fd, uint16_t events) override { return registerFd(fd, events); }

    bool deregisterFd(fd_t fd) override {
        auto it = index_.find(fd);
        if (it == index_.end()) return false;
        const size_t idx = it->second;
        const size_t last = fds_.size() - 1;
        if (idx != last) {
            fds_[idx] = fds_[last];
            index_[fds_[idx].fd] = idx;
        }
        fds_.pop_back();
        index_.erase(it);
        return true;
    }

    int poll(std::vector<PollResult>& out, int timeoutMs) override {
        const ULONG n = ::WSAPoll(fds_.data(), static_cast<ULONG>(fds_.size()), timeoutMs);
        if (n == ULONG(SOCKET_ERROR) || n == 0) return n == 0 ? 0 : -1;
        ULONG fired = 0;
        for (const WSAPOLLFD& p : fds_) {
            if (p.revents == 0) continue;
            out.push_back(PollResult{static_cast<fd_t>(p.fd), fromNativeEvents(p.revents)});
            if (++fired >= n) break;
        }
        return static_cast<int>(fired);
    }

    [[nodiscard]] const char* name() const override { return "wsapoll"; }

private:
    std::vector<WSAPOLLFD> fds_;
    std::map<fd_t, size_t> index_;
};

std::unique_ptr<Poller> createWsaPollPoller() { return std::make_unique<WsaPollPoller>(); }

} // namespace zenith::net

#endif // ZENITH_PLATFORM_WINDOWS
