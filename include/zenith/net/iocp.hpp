// ============================================================================
//  zenith::net::IocpServer —— Windows IOCP 传输层（Proactor）
//
//  为什么不塞进 Poller：IOCP 是"完成通知"，Poller 是"就绪通知"，接口语义不同。
//  硬把 IOCP 套进 reactor 接口只能做成假的就绪轮询，不如独立成一个传输层。
//
//  能力：监听 / AcceptEx 接入 / 每连接常驻 WSARecv / 线程安全发送 / 断开回调
//  未做：IDE 敏捷（AcceptEx 的零拷贝交接）、缓冲区池、环形发送队列限流
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/platform.hpp"

namespace zenith::net {

class ZENITH_API IocpServer : private core::NonCopyable {
public:
    /// 收到数据（worker 线程回调；不要在里面长时间阻塞）
    using DataCallback = std::function<void(uint64_t connId, const char* data, size_t n)>;
    /// 连接关闭（worker 线程回调）
    using CloseCallback = std::function<void(uint64_t connId)>;

    IocpServer();
    ~IocpServer();

    /// 当前平台是否支持（非 Windows 恒为 false）
    [[nodiscard]] static bool supported();

    bool listen(const std::string& host, uint16_t port);
    [[nodiscard]] uint16_t port() const noexcept;

    /// 启动 worker 线程（默认 1 个，够测试与轻量服务用）
    bool start(DataCallback onData, CloseCallback onClose, int workers = 1);

    /// 发送；可从任意线程调用（内部 PostQueuedCompletionStatus 排队）
    bool send(uint64_t connId, std::string data);

    void stop();

    [[nodiscard]] size_t connectionCount() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zenith::net
