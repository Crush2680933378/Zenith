// ============================================================================
//  zenith::mqtt::Broker —— MQTT 3.1.1 broker（QoS0/1 + 保留消息 + 通配订阅）
//
//  会话模型：每连接一个 Session；cleanSession=false 时订阅在断线后保留（简化版：
//  只保留订阅集合，不缓存离线消息）。QoS2 明确拒绝（CONNACK/SUBACK 里降级为 1）。
// ============================================================================
#pragma once

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/mqtt/mqtt.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"
#include "zenith/net/tcp_connection.hpp"
#include "zenith/net/tcp_server.hpp"

namespace zenith::mqtt {

class ZENITH_API Broker : private core::NonCopyable {
public:
    explicit Broker(net::EventLoop* loop, net::EventLoopThreadPool* ioPool = nullptr);
    ~Broker();

    /// 监听（端口 0 = 随机）；返回实际端口，失败返回 0
    uint16_t listen(const std::string& host, uint16_t port);
    void stop();

    [[nodiscard]] uint16_t port() const;
    [[nodiscard]] size_t clientCount() const;
    [[nodiscard]] size_t retainedCount() const;

private:
    struct Session {
        std::string clientId;
        bool cleanSession = true;
        std::vector<std::pair<std::string, uint8_t>> subscriptions;  // 过滤器 + 授予的 QoS
        std::string willTopic;
        std::string willMessage;
        uint16_t keepAlive = 0;
        uint16_t nextPacketId = 1;
    };
    using SessionPtr = std::shared_ptr<Session>;

    /// 锁内只改状态，发送/关闭排队到锁外执行。
    /// 直接 conn->send/forceClose 会重入 onClose（再拿同一把锁）→ 死锁。
    struct Pending {
        std::vector<std::pair<net::TcpConnection::Ptr, std::string>> sends;
        std::vector<net::TcpConnection::Ptr> closes;
    };

    void onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf);
    void onClose(const net::TcpConnection::Ptr& conn);
    void handlePacket(const net::TcpConnection::Ptr& conn, const SessionPtr& s, const Packet& p,
                      Pending& out);
    void handleConnect(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                       const ConnectPacket& c, Pending& out);
    void handlePublish(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                       const PublishPacket& p, Pending& out);
    void handleSubscribe(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                         const SubscribePacket& sub, Pending& out);
    void handleUnsubscribe(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                           const UnsubscribePacket& unsub, Pending& out);
    /// 把消息投给所有匹配的订阅者（调用方持锁）
    void routeLocked(const PublishPacket& p, const net::TcpConnection::Ptr& from, Pending& out);
    void sendTo(const net::TcpConnection::Ptr& conn, const SessionPtr& s, const PublishPacket& p,
                Pending& out);

    net::EventLoop* loop_;
    net::TcpServer server_;
    mutable std::mutex mtx_;
    std::map<const net::TcpConnection*, SessionPtr> sessions_;  // 连接 → 会话
    std::map<std::string, net::TcpConnection::Ptr> byClientId_;  // clientId → 连接
    std::map<std::string, PublishPacket> retained_;              // topic → 保留消息
    std::map<const net::TcpConnection*, std::string> inbound_;   // 每连接的解码缓冲
};

}  // namespace zenith::mqtt
