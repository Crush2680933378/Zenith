#include "zenith/redis/redis_client.hpp"

#include <atomic>

namespace zenith::redis {

// ---------------------------------------------------------------------------
// 编码
// ---------------------------------------------------------------------------
std::string encodeCommand(const std::vector<std::string>& args) {
    std::string out = "*" + std::to_string(args.size()) + "\r\n";
    for (const std::string& a : args) {
        out += "$" + std::to_string(a.size()) + "\r\n" + a + "\r\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// 解析器
// ---------------------------------------------------------------------------
bool RespParser::takeLine(const std::string& s, size_t& pos, std::string& line) {
    const size_t nl = s.find("\r\n", pos);
    if (nl == std::string::npos) return false;
    line.assign(s, pos, nl - pos);
    pos = nl + 2;
    return true;
}

RespParser::Result RespParser::feed(const uint8_t* data, size_t n) {
    pending_.append(reinterpret_cast<const char*>(data), n);
    size_t pos = 0;
    Result r = parseValue(pending_, pos, 0);
    if (r.state == State::Ok) {
        pending_.erase(0, pos);
    } else if (r.state == State::Error) {
        pending_.clear();
    }
    return r;
}

RespParser::Result RespParser::feed(core::Buffer& buf) {
    // 空缓冲也继续：pending_ 中可能已有完整回复（流水线连续解析）
    Result r = feed(buf.peek(), buf.readableBytes());
    if (r.state == State::Ok) {
        buf.consume(buf.readableBytes());
    }
    return r;
}

RespParser::Result RespParser::parseValue(const std::string& s, size_t& pos, int depth) {
    Result r;
    if (depth > 32) {
        r.state = State::Error;
        r.error = "nesting too deep";
        return r;
    }
    if (pos >= s.size()) return r;

    const char t = s[pos];
    ++pos;
    std::string line;
    switch (t) {
        case '+': // 简单字符串
        case '-': // 错误
        case ':': { // 整数
            if (!takeLine(s, pos, line)) return r; // Incomplete
            if (t == '+') {
                r.reply.type = RedisReply::Type::Simple;
                r.reply.str = std::move(line);
            } else if (t == '-') {
                r.reply.type = RedisReply::Type::Error;
                r.reply.str = std::move(line);
            } else {
                r.reply.type = RedisReply::Type::Integer;
                r.reply.integer = std::strtoll(line.c_str(), nullptr, 10);
            }
            r.state = State::Ok;
            return r;
        }
        case '$': { // 批量字符串
            if (!takeLine(s, pos, line)) return r;
            const int64_t len = std::strtoll(line.c_str(), nullptr, 10);
            if (len < 0) {
                r.reply.type = RedisReply::Type::Null;
                r.state = State::Ok;
                return r;
            }
            const size_t need = pos + static_cast<size_t>(len) + 2;
            if (s.size() < need) return r; // 数据不足：pos 由外层丢弃，下次重解
            r.reply.type = RedisReply::Type::Bulk;
            r.reply.str.assign(s, pos, static_cast<size_t>(len));
            pos = need;
            r.state = State::Ok;
            return r;
        }
        case '*': { // 数组
            if (!takeLine(s, pos, line)) return r;
            const int64_t count = std::strtoll(line.c_str(), nullptr, 10);
            if (count < 0) {
                r.reply.type = RedisReply::Type::Null;
                r.state = State::Ok;
                return r;
            }
            r.reply.type = RedisReply::Type::Array;
            r.reply.items.reserve(static_cast<size_t>(count));
            for (int64_t i = 0; i < count; ++i) {
                Result item = parseValue(s, pos, depth + 1);
                if (item.state != State::Ok) return Result{}; // Incomplete：整体重解
                r.reply.items.push_back(std::move(item.reply));
            }
            r.state = State::Ok;
            return r;
        }
        default:
            r.state = State::Error;
            r.error = std::string("bad RESP type '") + t + "'";
            return r;
    }
}

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------
RedisClient::RedisClient(net::EventLoop* loop) : loop_(loop) {}

void RedisClient::connect(const std::string& host, uint16_t port,
                           std::function<void()> onReady,
                           std::function<void(const std::string&)> onFail) {
    auto client = std::make_shared<net::TcpClient>(loop_);
    net::ConnectOptions copt;
    copt.host = host;
    copt.port = port;
    copt.timeoutMs = 4000;

    auto self = this;
    client->connect(
        copt,
        [self, onReady](const net::TcpConnection::Ptr& conn) {
            self->conn_ = conn;
            conn->setMessageCallback(
                [self](const net::TcpConnection::Ptr&, core::Buffer& buf) {
                    self->handleIncoming(buf);
                });
            if (onReady) onReady(); // Redis 无握手，连接即就绪
        },
        [onFail](const std::string& reason) {
            if (onFail) onFail("redis connect failed: " + reason);
        });
    // client 由 TcpClient 内部 shared_from_this 保活至连接判定完成
    (void)client;
}

void RedisClient::command(std::vector<std::string> args, ReplyCallback cb) {
    if (!conn_) return; // 未连接：静默丢弃（调用方应保证先 connect）
    conn_->send(encodeCommand(args));
    pending_.push_back(std::move(cb));
}

void RedisClient::get(const std::string& key, ReplyCallback cb) {
    command({"GET", key}, std::move(cb));
}
void RedisClient::set(const std::string& key, const std::string& value, ReplyCallback cb) {
    command({"SET", key, value}, std::move(cb));
}
void RedisClient::del(const std::string& key, ReplyCallback cb) {
    command({"DEL", key}, std::move(cb));
}

void RedisClient::handleIncoming(core::Buffer& buf) {
    for (;;) {
        RespParser::Result r = parser_.feed(buf);
        if (r.state == RespParser::State::Incomplete) return;
        if (r.state == RespParser::State::Error) {
            // 协议错位：丢弃对齐并回调错误
            if (!pending_.empty()) {
                auto cb = std::move(pending_.front());
                pending_.pop_front();
                if (cb) {
                    cb(core::Result<RedisReply>::Err(
                        core::Error(60, "resp error: " + r.error)));
                }
            }
            return;
        }
        if (pending_.empty()) return; // 服务端主动推送（如订阅）：M3 忽略
        auto cb = std::move(pending_.front());
        pending_.pop_front();
        if (cb) cb(core::Result<RedisReply>::Ok(std::move(r.reply)));
    }
}

core::Result<RedisReply> RedisClient::commandBlocking(const std::string& host, uint16_t port,
                                                        std::vector<std::string> args,
                                                        int timeoutMs) {
    net::EventLoop loop; // 调用线程上的临时 loop
    core::Result<RedisReply> result =
        core::Result<RedisReply>::Err(core::Error(61, "not started"));
    std::atomic<bool> got{false};

    {
        RedisClient client(&loop);
        loop.queueInLoop([&] {
            client.connect(host, port,
                           [&client, &loop, &args, &result, &got] {
                               client.command(std::move(args),
                                              [&loop, &result, &got](core::Result<RedisReply> r) {
                                                  result = std::move(r);
                                                  got.store(true, std::memory_order_release);
                                                  loop.stop();
                                              });
                           },
                           [&loop, &result, &got](const std::string& reason) {
                               result = core::Result<RedisReply>::Err(core::Error(62, reason));
                               got.store(true, std::memory_order_release);
                               loop.stop();
                           });
        });
        loop.scheduleAfter(std::chrono::milliseconds(timeoutMs),
                           [&loop, &result, &got] {
                               if (got.load()) return;
                               result = core::Result<RedisReply>::Err(
                                   core::Error(63, "redis command timeout"));
                               got.store(true, std::memory_order_release);
                               loop.stop();
                           });
        loop.loop();
    }
    if (!got.load(std::memory_order_acquire)) {
        return core::Result<RedisReply>::Err(core::Error(64, "internal: no result"));
    }
    return result;
}

} // namespace zenith::redis
