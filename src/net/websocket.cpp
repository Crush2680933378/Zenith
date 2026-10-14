#include "zenith/net/websocket.hpp"

#include <atomic>
#include <random>

#include "zenith/crypto/base64.hpp"
#include "zenith/crypto/sha1.hpp"
#include "zenith/http/http_parser.hpp"

namespace zenith::net {

namespace {
constexpr const char* kWsGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void appendUint16(std::string& s, uint16_t v) {
    s.push_back(static_cast<char>((v >> 8) & 0xFF));
    s.push_back(static_cast<char>(v & 0xFF));
}

uint32_t randomMaskKey() {
    static std::mt19937 rng{std::random_device{}()};
    return rng();
}

void applyMask(uint8_t* data, size_t n, uint32_t key) {
    const uint8_t k[4] = {static_cast<uint8_t>(key >> 24), static_cast<uint8_t>(key >> 16),
                          static_cast<uint8_t>(key >> 8), static_cast<uint8_t>(key)};
    for (size_t i = 0; i < n; ++i) data[i] ^= k[i & 3];
}
} // namespace

std::string WsFrame::encode(bool mask, uint32_t maskKey) const {
    std::string out;
    out.reserve(2 + payload.size() + (mask ? 4 : 0));
    out.push_back(static_cast<char>((fin ? 0x80 : 0x00) | (static_cast<uint8_t>(opcode) & 0x0F)));
    const size_t n = payload.size();
    uint8_t lenByte = mask ? 0x80 : 0x00;
    if (n <= 125) {
        lenByte |= static_cast<uint8_t>(n);
        out.push_back(static_cast<char>(lenByte));
    } else if (n <= 0xFFFF) {
        lenByte |= 126;
        out.push_back(static_cast<char>(lenByte));
        appendUint16(out, static_cast<uint16_t>(n));
    } else {
        lenByte |= 127;
        out.push_back(static_cast<char>(lenByte));
        for (int i = 7; i >= 0; --i) {
            out.push_back(static_cast<char>((static_cast<uint64_t>(n) >> (8 * i)) & 0xFF));
        }
    }
    if (mask) {
        out.push_back(static_cast<char>(maskKey >> 24));
        out.push_back(static_cast<char>(maskKey >> 16));
        out.push_back(static_cast<char>(maskKey >> 8));
        out.push_back(static_cast<char>(maskKey));
    }
    out += payload;
    if (mask) {
        applyMask(reinterpret_cast<uint8_t*>(out.data()) + out.size() - n, n, maskKey);
    }
    return out;
}

WsDecodeResult wsDecodeFrame(const uint8_t* data, size_t n, bool requireMask) {
    WsDecodeResult r;
    if (n < 2) return r;
    const uint8_t b0 = data[0];
    const uint8_t b1 = data[1];
    const bool fin = (b0 & 0x80) != 0;
    const bool rsv = (b0 & 0x70) != 0;
    const uint8_t opcode = b0 & 0x0F;
    const bool masked = (b1 & 0x80) != 0;
    uint64_t len = b1 & 0x7F;

    if (rsv) {
        r.state = WsDecodeResult::State::Error;
        r.error = "RSV bits set";
        return r;
    }
    if (requireMask && !masked) {
        r.state = WsDecodeResult::State::Error;
        r.error = "client frame not masked";
        return r;
    }

    size_t pos = 2;
    if (len == 126) {
        if (n < pos + 2) return r;
        len = (static_cast<uint64_t>(data[pos]) << 8) | data[pos + 1];
        pos += 2;
    } else if (len == 127) {
        if (n < pos + 8) return r;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | data[pos + i];
        pos += 8;
        if (len > (1ull << 31)) { // 上限 2GB 防滥用
            r.state = WsDecodeResult::State::Error;
            r.error = "frame too large";
            return r;
        }
    }
    uint32_t maskKey = 0;
    if (masked) {
        if (n < pos + 4) return r;
        maskKey = (static_cast<uint32_t>(data[pos]) << 24) |
                  (static_cast<uint32_t>(data[pos + 1]) << 16) |
                  (static_cast<uint32_t>(data[pos + 2]) << 8) |
                  static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
    }
    if (n < pos + len) return r; // 数据不足

    r.frame.fin = fin;
    r.frame.opcode = static_cast<WsOpcode>(opcode);
    r.frame.payload.assign(reinterpret_cast<const char*>(data) + pos, static_cast<size_t>(len));
    if (masked) {
        applyMask(reinterpret_cast<uint8_t*>(r.frame.payload.data()), r.frame.payload.size(),
                  maskKey);
    }
    r.consumed = pos + static_cast<size_t>(len);
    r.state = WsDecodeResult::State::Ok;
    return r;
}

std::string wsComputeAccept(const std::string& clientKey) {
    crypto::Sha1 sha;
    sha.update(clientKey);
    sha.update(kWsGuid);
    uint8_t digest[20];
    sha.final(digest);
    return crypto::base64Encode(digest, 20);
}

/* ==========================================================================
 * WebSocketSession
 * ======================================================================== */

WebSocketSession::WebSocketSession(TcpConnection::Ptr conn, bool clientSide)
    : conn_(std::move(conn)), clientSide_(clientSide) {}

WebSocketSession::~WebSocketSession() = default;

WebSocketSession::Ptr WebSocketSession::takeover(TcpConnection::Ptr conn, bool clientSide) {
    Ptr session(new WebSocketSession(std::move(conn), clientSide));
    // 接管消息回调：后续字节流均为 WebSocket 帧
    session->conn_->setMessageCallback(
        [s = session->shared_from_this()](const TcpConnection::Ptr&, core::Buffer& buf) {
            s->handleIncoming(buf);
        });
    return session;
}

void WebSocketSession::handleIncoming(core::Buffer& buf) {
    for (;;) {
        WsDecodeResult r =
            wsDecodeFrame(buf.peek(), buf.readableBytes(), /*requireMask=*/!clientSide_);
        if (r.state == WsDecodeResult::State::Incomplete) return;
        if (r.state == WsDecodeResult::State::Error) {
            finishClose(1002, r.error);
            return;
        }
        buf.consume(r.consumed);
        handleFrame(std::move(r.frame));
        if (closed_) return;
    }
}

void WebSocketSession::handleFrame(WsFrame&& f) {
    switch (f.opcode) {
        case WsOpcode::Ping:
            sendFrame(WsOpcode::Pong, f.payload);
            return;
        case WsOpcode::Pong:
            return; // 心跳应答：用户可订阅，M3 忽略
        case WsOpcode::Close: {
            uint16_t code = 1005;
            if (f.payload.size() >= 2) {
                code = (static_cast<uint8_t>(f.payload[0]) << 8) |
                       static_cast<uint8_t>(f.payload[1]);
            }
            if (!closeSent_) {
                // 对端发起关闭：回 close 帧完成握手
                sendFrame(WsOpcode::Close, f.payload);
            }
            finishClose(code, "");
            return;
        }
        case WsOpcode::Text:
        case WsOpcode::Binary:
            if (fragmenting_) {
                finishClose(1002, "new data frame during fragmentation");
                return;
            }
            if (f.fin) {
                if (onMessage_) onMessage_(shared_from_this(), std::move(f.payload),
                                           f.opcode == WsOpcode::Text);
            } else {
                fragmenting_ = true;
                fragOpcode_ = f.opcode;
                fragPayload_ = std::move(f.payload);
            }
            return;
        case WsOpcode::Continuation:
            if (!fragmenting_) {
                finishClose(1002, "continuation without start frame");
                return;
            }
            fragPayload_ += f.payload;
            if (fragPayload_.size() > (32ull << 20)) { // 32MB 消息上限
                finishClose(1009, "message too large");
                return;
            }
            if (f.fin) {
                fragmenting_ = false;
                std::string full = std::move(fragPayload_);
                fragPayload_.clear();
                if (onMessage_) {
                    onMessage_(shared_from_this(), std::move(full), fragOpcode_ == WsOpcode::Text);
                }
            }
            return;
        default:
            finishClose(1002, "reserved opcode");
            return;
    }
}

void WebSocketSession::sendText(const std::string& s) { sendFrame(WsOpcode::Text, s); }
void WebSocketSession::sendBinary(const std::string& s) { sendFrame(WsOpcode::Binary, s); }
void WebSocketSession::ping(const std::string& payload) { sendFrame(WsOpcode::Ping, payload); }

void WebSocketSession::processPending(const std::string& bytes) {
    if (bytes.empty() || closed_) return;
    core::Buffer buf;
    buf.appendString(bytes);
    handleIncoming(buf);
}

void WebSocketSession::close(uint16_t code, const std::string& reason) {
    if (closed_ || closeSent_) return;
    std::string payload;
    payload.push_back(static_cast<char>((code >> 8) & 0xFF));
    payload.push_back(static_cast<char>(code & 0xFF));
    payload += reason;
    sendFrame(WsOpcode::Close, payload);
    closeSent_ = true;
    // 半关闭写侧，等待对端 close（或直接超时由 conn 关闭兜底）
    conn_->shutdown();
}

void WebSocketSession::sendFrame(WsOpcode op, const std::string& payload) {
    if (closed_) return;
    WsFrame f;
    f.fin = true;
    f.opcode = op;
    f.payload = payload;
    conn_->send(f.encode(/*mask=*/clientSide_, randomMaskKey()));
}

void WebSocketSession::finishClose(uint16_t code, const std::string& reason) {
    if (closed_) return;
    closed_ = true;
    (void)code;
    (void)reason;
    if (onClose_) {
        auto cb = std::move(onClose_);
        cb(shared_from_this());
    }
    conn_->forceClose();
}

/* ==========================================================================
 * WebSocketClient
 * ======================================================================== */

struct WebSocketClient::Pending {
    http::HttpResponseParser parser;
    OpenCallback onOpen;
    FailCallback onFail;
    TcpConnection::Ptr conn;
    net::EventLoop* loop = nullptr;
    uint64_t timerId = 0;
    bool done = false;
};

void WebSocketClient::connect(net::EventLoop* loop, const WsConnectOptions& opt,
                               OpenCallback onOpen, FailCallback onFail) {
    auto st = std::make_shared<Pending>();
    st->onOpen = std::move(onOpen);
    st->onFail = std::move(onFail);
    st->loop = loop;

    client_ = std::make_shared<TcpClient>(loop);

    // 生成随机 client key（16 字节 base64）
    std::string key;
    {
        std::mt19937 rng{std::random_device{}()};
        std::string raw(16, '\0');
        for (auto& c : raw) c = static_cast<char>(rng() & 0xFF);
        key = crypto::base64Encode(raw);
    }
    const std::string request = [&] {
        std::string s = "GET " + opt.path + " HTTP/1.1\r\nHost: " + opt.host;
        if (opt.port != 80 && opt.port != 0) {
            s += ":" + std::to_string(opt.port);
        }
        s += "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
             "\r\nSec-WebSocket-Version: 13\r\n";
        if (!opt.subprotocol.empty()) s += "Sec-WebSocket-Protocol: " + opt.subprotocol + "\r\n";
        s += "\r\n";
        return s;
    }();
    const std::string expectedAccept = wsComputeAccept(key);

    ConnectOptions copt;
    copt.host = opt.host;
    copt.port = opt.port;
    copt.timeoutMs = opt.timeoutMs;

    client_->connect(
        copt,
        [st, loop, request, expectedAccept, timeoutMs = opt.timeoutMs](
            const TcpConnection::Ptr& conn) {
            st->conn = conn;
            st->timerId = loop->scheduleAfter(std::chrono::milliseconds(timeoutMs), [st] {
                if (st->done) return;
                st->done = true;
                if (st->conn) st->conn->forceClose();
                if (st->onFail) st->onFail("websocket upgrade timeout");
            });
            conn->setMessageCallback(
                [st, expectedAccept](const TcpConnection::Ptr&, core::Buffer& buf) {
                    auto out = st->parser.feed(buf);
                    if (out.failed()) {
                        if (!st->done) {
                            st->done = true;
                            if (st->conn) st->conn->forceClose();
                            if (st->onFail) st->onFail("bad upgrade response");
                        }
                        return;
                    }
                    if (!out.complete()) return;
                    const http::HttpResponse& resp = st->parser.response();
                    const bool upgraded = resp.status == 101 &&
                        resp.headers.get("Upgrade") == "websocket" &&
                        resp.headers.get("Sec-WebSocket-Accept") == expectedAccept;
                    if (!upgraded) {
                        if (!st->done) {
                            st->done = true;
                            if (st->conn) st->conn->forceClose();
                            if (st->onFail) {
                                st->onFail("upgrade rejected (status=" +
                                           std::to_string(resp.status) + ")");
                            }
                        }
                        return;
                    }
                    if (st->done) return;
                    st->done = true;
                    // 升级响应后可能紧跟早期 WS 帧：取出残留交给会话。
                    // 关键：takeover 会替换正在执行的这条消息回调（其闭包随即销毁），
                    // 在自身回调内继续访问捕获即 use-after-free → 投递到下轮任务执行。
                    const std::string leftover = st->parser.releaseLeftover();
                    net::EventLoop* wsLoop = st->loop;
                    wsLoop->queueInLoop([st, leftover]() mutable {
                        auto session = WebSocketSession::takeover(st->conn, /*clientSide=*/true);
                        session->processPending(leftover);
                        if (st->onOpen) st->onOpen(session);
                    });
                });
            conn->send(request);
        },
        [st](const std::string& reason) {
            if (st->done) return;
            st->done = true;
            if (st->onFail) st->onFail("connect failed: " + reason);
        });
}

void WebSocketClient::fail(const std::string& reason) {
    // 占位：当前失败路径均直接回调
    (void)reason;
}

} // namespace zenith::net
