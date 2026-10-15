// ============================================================================
//  zenith::net::websocket —— RFC 6455 实现（帧编解码 + 会话 + 客户端）
//  - 服务端：HttpServer 注册升级路径 → 101 握手 → WebSocketSession 接管连接
//  - 客户端：WebSocketClient 发起升级并收发帧（客户端帧强制掩码）
//  - 自动处理：ping→pong、close 握手、分片消息重组、大帧(64 位长度)
// ============================================================================
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "zenith/core/buffer.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/tcp_client.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::net {

/// WebSocket 操作码
enum class WsOpcode : uint8_t {
    Continuation = 0x0,
    Text         = 0x1,
    Binary       = 0x2,
    Close        = 0x8,
    Ping         = 0x9,
    Pong         = 0xA,
};

/// 单个帧（已解掩码 / 未编码）
struct WsFrame {
    bool      fin = true;
    WsOpcode  opcode = WsOpcode::Text;
    std::string payload;

    /// 服务端发出（不掩码）
    [[nodiscard]] std::string encode(bool mask, uint32_t maskKey = 0) const;
};

/// 从 buffer 解出一帧；数据不足/协议错误分别返回 Incomplete / Error。
struct WsDecodeResult {
    enum class State { Incomplete, Ok, Error };
    State state = State::Incomplete;
    std::string error;
    WsFrame frame;
    size_t consumed = 0; // Ok 时：本帧占用字节数
};
ZENITH_API WsDecodeResult wsDecodeFrame(const uint8_t* data, size_t n, bool requireMask);

/// RFC 6455 握手 Accept 计算：base64(SHA1(key + GUID))
ZENITH_API std::string wsComputeAccept(const std::string& clientKey);

// ---------------------------------------------------------------------------
// 会话（服务端/客户端共用；持有 TcpConnection）
// ---------------------------------------------------------------------------
class ZENITH_API WebSocketSession
    : public std::enable_shared_from_this<WebSocketSession> {
public:
    using Ptr = std::shared_ptr<WebSocketSession>;
    using MessageCallback = std::function<void(const Ptr&, std::string, bool isText)>;
    using CloseCallback   = std::function<void(const Ptr&)>;

    /// 服务端：接管既有 TcpConnection（HttpServer 升级路径内部使用）
    static Ptr takeover(TcpConnection::Ptr conn, bool clientSide = false);
    ~WebSocketSession();

    void setMessageCallback(MessageCallback cb) { onMessage_ = std::move(cb); }
    void setCloseCallback(CloseCallback cb)     { onClose_ = std::move(cb); }
    void setOpenCallback(std::function<void(const Ptr&)> cb) { onOpen_ = std::move(cb); }

    void sendText(const std::string& s);
    void sendBinary(const std::string& s);
    void ping(const std::string& payload = "");
    /// 发起 close 握手（发送 close 帧并半关闭）
    void close(uint16_t code = 1000, const std::string& reason = "");

    /// 外部喂入字节流（升级响应之后的残留帧等）；线程不安全，仅同 loop。
    void processPending(const std::string& bytes);

    [[nodiscard]] const std::string& peer() const noexcept { return conn_->peer(); }
    [[nodiscard]] TcpConnection* connection() const noexcept { return conn_.get(); }
    /// 用户附带状态
    void setContext(std::shared_ptr<void> ctx) { context_ = std::move(ctx); }
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> context() const {
        return std::static_pointer_cast<T>(context_);
    }

private:
    explicit WebSocketSession(TcpConnection::Ptr conn, bool clientSide);
    void handleIncoming(core::Buffer& buf);
    void handleFrame(WsFrame&& f);
    void sendFrame(WsOpcode op, const std::string& payload);
    void finishClose(uint16_t code, const std::string& reason);

    TcpConnection::Ptr conn_;
    bool clientSide_;
    bool closed_ = false;
    bool closeSent_ = false;

    // 分片重组
    bool fragmenting_ = false;
    WsOpcode fragOpcode_ = WsOpcode::Text;
    std::string fragPayload_;

    std::function<void(const Ptr&)> onOpen_;
    MessageCallback onMessage_;
    CloseCallback onClose_;
    std::shared_ptr<void> context_;
};

// ---------------------------------------------------------------------------
// 客户端：HTTP Upgrade + 帧流
// ---------------------------------------------------------------------------
struct WsConnectOptions {
    std::string host{"127.0.0.1"};
    uint16_t port = 80;
    std::string path{"/"};
    std::string subprotocol; // 可选 Sec-WebSocket-Protocol
    int timeoutMs = 5000;
    bool tls = false;        // wss://（M5.1）
    std::string caFile;      // 自定 CA（空 = 系统信任）
};

class ZENITH_API WebSocketClient {
public:
    using OpenCallback  = std::function<void(const WebSocketSession::Ptr&)>;
    using FailCallback  = std::function<void(const std::string&)>;

    /// 连接并升级（须以 make_shared 持有）。回调在 loop 线程触发。
    void connect(net::EventLoop* loop, const WsConnectOptions& opt,
                 OpenCallback onOpen, FailCallback onFail);

private:
    void fail(const std::string& reason);
    std::shared_ptr<class TcpClient> client_;
    struct Pending;
    std::shared_ptr<Pending> pending_;
};

} // namespace zenith::net
