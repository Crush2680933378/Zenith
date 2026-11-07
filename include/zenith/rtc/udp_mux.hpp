// ============================================================================
//  zenith::rtc::UdpMux —— 一条 UDP 链路上的协议分流（WebRTC 的 demux）
//
//  WebRTC 在**同一条 UDP 5 元组**上同时承载：
//    - STUN（ICE 连通性检查）：首字节 0..3
//    - DTLS（握手与密钥协商）：  首字节 20..63
//    - RTP/RTCP（媒体）：        首字节 128..191
//  不做分流，三者的收包线程会互相抢包——这正是把 DTLS 直接挂到 ICE 的 socket 上
//  跑不通的原因（见 dtls.hpp 的边界说明）。
//
//  这一层只做「收一个包 → 按首字节投递给对应回调」，不解析协议、不持有会话状态。
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/rtc/stun.hpp"

namespace zenith::rtc {

/// 首字节 → 协议类别
enum class DatagramKind {
    Stun,
    Dtls,
    Rtp,
    Unknown,
};

ZENITH_API DatagramKind classifyDatagram(const char* data, size_t n);

class ZENITH_API UdpMux : private core::NonCopyable {
public:
    /// 回调：载荷 + 来源地址
    using Handler = std::function<void(std::string_view payload, const StunAddress& from)>;

    UdpMux();
    ~UdpMux();

    /// 绑定本地 UDP 端口（0 = 随机，绑定到 127.0.0.1）
    bool bind(uint16_t port = 0);
    [[nodiscard]] uint16_t localPort() const;
    [[nodiscard]] int socketFd() const;

    void onStun(Handler h);
    void onDtls(Handler h);
    void onRtp(Handler h);
    /// 未识别的首字节（调试用；生产里应当丢弃）
    void onUnknown(Handler h);

    /// 收一轮包（阻塞至多 timeoutMs），按类别分发；返回本轮处理的包数
    int pollOnce(int timeoutMs = 100);

    /// 发送一个数据报
    bool sendTo(const StunAddress& to, std::string_view data);

    [[nodiscard]] size_t dropped() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zenith::rtc
