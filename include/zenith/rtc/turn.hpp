// ============================================================================
//  zenith::rtc —— TURN（RFC 5766）客户端与服务端（简化版，无鉴权）
//
//  作用：NAT 后面的两端拿不到直连地址时，通过中继服务器转发媒体。
//  这一版只实现 UDP 中继的核心三条：Allocate / CreatePermission / Send·Data。
//
//  已做：Allocate（RELAYED-ADDRESS + LIFETIME）、CreatePermission、Send/Data indication、
//        多客户端各自独立 relay socket
//  未做：**鉴权**（long-term credential / MESSAGE-INTEGRITY / NONCE）、ChannelData、
//        TCP/TLS 承载、Refresh 续租、端口分配策略
//
//  因为不做鉴权，这个实现只能用于本机/可信网络；对外部署前必须补鉴权，
//  否则等于开放中继（reflection/amplification 风险）。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/rtc/stun.hpp"

namespace zenith::rtc {

class ZENITH_API TurnClient : private core::NonCopyable {
public:
    TurnClient();
    ~TurnClient();

    /// 向 TURN 服务器申请中继地址
    bool allocate(const std::string& serverIp, uint16_t serverPort, int timeoutMs = 2000);
    [[nodiscard]] StunAddress relayedAddress() const;
    [[nodiscard]] uint16_t localPort() const;

    /// 允许 peer 往中继地址发数据（TURN 要求先建权限）
    bool createPermission(const StunAddress& peer, int timeoutMs = 2000);

    /// 通过中继给 peer 发数据（Send indication）
    bool sendTo(const StunAddress& peer, std::string_view data);

    /// 收一条数据；from 为对端地址。超时/失败返回空串
    std::string receive(StunAddress& from, int timeoutMs = 2000);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class ZENITH_API TurnServer : private core::NonCopyable {
public:
    TurnServer();
    ~TurnServer();

    /// 监听（127.0.0.1，port=0 随机）
    bool listen(uint16_t port = 0);
    [[nodiscard]] uint16_t port() const;

    /// 泵一轮：处理一个到达的客户端包或 relay 数据（阻塞至多 timeoutMs）
    void pollOnce(int timeoutMs = 100);

    /// 当前分配数
    [[nodiscard]] size_t allocations() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zenith::rtc
