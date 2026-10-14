// ============================================================================
//  zenith::net::Poller —— 就绪通知后端抽象（Reactor 的核心接口）
//  后端优先级：epoll(Linux) > kqueue(macOS) > WSAPoll(Windows) > select(通用)
//  IOCP 为 Proactor 模型，接口形态不同，规划在 M4 以独立 ITransport 形态接入。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/net/socket.hpp" // fd_t

namespace zenith::net {

enum PollEvent : uint16_t {
    kPollNone     = 0,
    kPollReadable = 1u << 0,
    kPollWritable = 1u << 1,
    kPollError    = 1u << 2,
    kPollHangup   = 1u << 3,
};

struct PollResult {
    fd_t     fd = kInvalidFd;
    uint16_t revents = kPollNone;
};

class Poller {
public:
    virtual ~Poller() = default;

    virtual bool registerFd(fd_t fd, uint16_t events) = 0;
    virtual bool updateFd(fd_t fd, uint16_t events)   = 0;
    virtual bool deregisterFd(fd_t fd)                = 0;

    /// 阻塞等待就绪（timeoutMs < 0 表示无限期）。返回就绪条数；out 追加结果。
    virtual int poll(std::vector<PollResult>& out, int timeoutMs) = 0;

    [[nodiscard]] virtual const char* name() const = 0;
};

/// 按平台选择最优后端。
ZENITH_API std::unique_ptr<Poller> createPoller();

} // namespace zenith::net
