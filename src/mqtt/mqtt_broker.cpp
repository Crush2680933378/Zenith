// ============================================================================
//  MQTT broker（QoS0/1 + 保留消息 + 通配订阅）
//
//  并发模型：所有状态由 mtx_ 保护；**发送与关闭一律排队到锁外执行**——
//  在锁内调用 conn->send()/forceClose() 会重入 onClose（同一把锁）导致死锁，
//  且关闭时 erase 会破坏正在遍历的 sessions_。
// ============================================================================
#include "zenith/mqtt/mqtt_broker.hpp"

#include <algorithm>
#include <atomic>

namespace zenith::mqtt {
namespace {
std::string randomClientId() {
    static std::atomic<uint64_t> seq{0};
    return "auto-" + std::to_string(seq.fetch_add(1));
}
}  // namespace

Broker::Broker(net::EventLoop* loop, net::EventLoopThreadPool* ioPool)
    : loop_(loop), server_(loop, ioPool) {
    server_.setMessageCallback([this](const net::TcpConnection::Ptr& c, core::Buffer& buf) {
        onMessage(c, buf);
    });
    server_.addConnectionDecorator([this](const net::TcpConnection::Ptr& conn) {
        conn->setCloseCallback([this](const net::TcpConnection::Ptr& c) { onClose(c); });
    });
}

Broker::~Broker() { stop(); }

uint16_t Broker::listen(const std::string& host, uint16_t port) {
    auto r = server_.listen(host, port);
    return r ? server_.port() : 0;
}

void Broker::stop() { server_.stop(); }

uint16_t Broker::port() const { return server_.port(); }

size_t Broker::clientCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return byClientId_.size();
}

size_t Broker::retainedCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return retained_.size();
}

void Broker::onClose(const net::TcpConnection::Ptr& conn) {
    std::lock_guard<std::mutex> lk(mtx_);
    auto it = sessions_.find(conn.get());
    if (it != sessions_.end()) {
        const SessionPtr s = it->second;
        if (!s->clientId.empty()) byClientId_.erase(s->clientId);
        sessions_.erase(it);
    }
    inbound_.erase(conn.get());
}

void Broker::onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
    Pending pending;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        std::string& in = inbound_[conn.get()];
        in.append(reinterpret_cast<const char*>(buf.peek()), buf.readableBytes());
        buf.consume(buf.readableBytes());

        for (;;) {
            auto r = decode(in);
            if (r.status == DecodeResult::Status::Pending) break;
            if (r.status == DecodeResult::Status::Error) {
                in.clear();
                pending.closes.push_back(conn);  // 协议错误直接断，避免半截包继续污染
                break;
            }
            in.erase(0, r.consumed);
            auto sit = sessions_.find(conn.get());
            if (sit == sessions_.end()) {
                if (r.packet.type != PacketType::Connect) {
                    pending.closes.push_back(conn);  // 第一个包必须是 CONNECT
                    break;
                }
                auto s = std::make_shared<Session>();
                sessions_[conn.get()] = s;
                handleConnect(conn, s, r.packet.connect, pending);
                continue;
            }
            handlePacket(conn, sit->second, r.packet, pending);
            if (r.packet.type == PacketType::Disconnect) {
                pending.closes.push_back(conn);
                break;
            }
        }
    }

    for (auto& [c, data] : pending.sends) c->send(data);
    for (auto& c : pending.closes) c->forceClose();
}

void Broker::handleConnect(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                           const ConnectPacket& c, Pending& out) {
    s->clientId = c.clientId.empty() ? randomClientId() : c.clientId;
    s->cleanSession = c.cleanSession;
    s->keepAlive = c.keepAlive;
    s->willTopic = c.willTopic;
    s->willMessage = c.willMessage;

    // 同 clientId 的旧连接：按 3.1.1 语义踢掉
    auto old = byClientId_.find(s->clientId);
    if (old != byClientId_.end() && old->second.get() != conn.get()) {
        out.closes.push_back(old->second);
    }
    if (s->cleanSession) s->subscriptions.clear();
    byClientId_[s->clientId] = conn;
    out.sends.emplace_back(conn, encodeConnack(false, ConnectReturn::Accepted));
}

void Broker::handlePacket(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                          const Packet& p, Pending& out) {
    switch (p.type) {
        case PacketType::Publish:
            handlePublish(conn, s, p.publish, out);
            break;
        case PacketType::Subscribe:
            handleSubscribe(conn, s, p.subscribe, out);
            break;
        case PacketType::Unsubscribe:
            handleUnsubscribe(conn, s, p.unsubscribe, out);
            break;
        case PacketType::Pingreq:
            out.sends.emplace_back(conn, encodePingresp());
            break;
        case PacketType::Disconnect:
            s->willTopic.clear();  // 正常断开：不发布遗嘱
            break;
        default:
            break;
    }
}

void Broker::handlePublish(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                           const PublishPacket& p, Pending& out) {
    (void)s;
    if (!validTopicName(p.topic)) return;
    if (p.retain) {
        if (p.payload.empty()) {
            retained_.erase(p.topic);  // 空载荷 = 清除保留消息
        } else {
            retained_[p.topic] = p;
        }
    }
    routeLocked(p, conn, out);
    if (p.qos == 1 && p.packetId != 0) out.sends.emplace_back(conn, encodePuback(p.packetId));
}

void Broker::routeLocked(const PublishPacket& p, const net::TcpConnection::Ptr& from,
                         Pending& out) {
    (void)from;
    for (auto& [key, s] : sessions_) {
        auto cit = byClientId_.find(s->clientId);
        if (cit == byClientId_.end()) continue;
        bool matched = false;
        for (const auto& [filter, qos] : s->subscriptions) {
            (void)qos;
            if (topicMatches(filter, p.topic)) {
                matched = true;
                break;
            }
        }
        if (!matched) continue;
        sendTo(cit->second, s, p, out);
    }
}

void Broker::sendTo(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                    const PublishPacket& p, Pending& out) {
    PublishPacket msg;
    msg.topic = p.topic;
    msg.payload = p.payload;
    msg.qos = p.qos;
    if (msg.qos > 0) {
        msg.packetId = s->nextPacketId++;
        if (s->nextPacketId == 0) s->nextPacketId = 1;
    }
    out.sends.emplace_back(conn, encodePublish(msg));
}

void Broker::handleSubscribe(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                             const SubscribePacket& sub, Pending& out) {
    std::vector<uint8_t> granted;
    for (const auto& [filter, qos] : sub.filters) {
        if (!validTopicFilter(filter)) {
            granted.push_back(0x80);  // failure
            continue;
        }
        s->subscriptions.emplace_back(filter, qos);
        granted.push_back(qos);
        // 保留消息：订阅成功立即投递（retain 标志转发时清零）
        for (const auto& [topic, msg] : retained_) {
            if (!topicMatches(filter, topic)) continue;
            PublishPacket copy = msg;
            copy.qos = qos;
            copy.retain = false;
            if (copy.qos > 0) {
                copy.packetId = s->nextPacketId++;
                if (s->nextPacketId == 0) s->nextPacketId = 1;
            }
            out.sends.emplace_back(conn, encodePublish(copy));
        }
    }
    out.sends.emplace_back(conn, encodeSuback(sub.packetId, granted));
}

void Broker::handleUnsubscribe(const net::TcpConnection::Ptr& conn, const SessionPtr& s,
                               const UnsubscribePacket& unsub, Pending& out) {
    for (const auto& f : unsub.filters) {
        s->subscriptions.erase(
            std::remove_if(s->subscriptions.begin(), s->subscriptions.end(),
                           [&f](const auto& e) { return e.first == f; }),
            s->subscriptions.end());
    }
    out.sends.emplace_back(conn, encodeUnsuback(unsub.packetId));
}

}  // namespace zenith::mqtt
