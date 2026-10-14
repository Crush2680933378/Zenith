// ============================================================================
//  zenith::redis —— 自研 Redis RESP2 协议客户端
//  - 增量解析器（任意字节切分）、命令流水线（回调 FIFO 对齐）
//  - 异步回调形态 + fetchBlocking 便捷形态；协程 awaiter 见 coro/redis_awaiters
//  - 测试通过自研 mock Redis 服务器（tests 内置）验证，无需真实 Redis
// ============================================================================
#pragma once

#include <deque>
#include <functional>
#include <memory>

#include "zenith/core/buffer.hpp"
#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/tcp_client.hpp"
#include "zenith/net/tcp_connection.hpp"

namespace zenith::redis {

/// RESP 回复值
struct RedisReply {
    enum class Type : uint8_t { Simple, Error, Integer, Bulk, Array, Null };
    Type type = Type::Null;
    std::string str;                 // Simple/Error/Bulk
    int64_t integer = 0;             // Integer
    std::vector<RedisReply> items;   // Array

    [[nodiscard]] bool isError() const noexcept { return type == Type::Error; }
    [[nodiscard]] bool isNull() const noexcept { return type == Type::Null; }
    [[nodiscard]] std::string toString() const {
        return type == Type::Bulk || type == Type::Simple || type == Type::Error ? str
                 : type == Type::Integer ? std::to_string(integer)
                                         : std::string("(nil)");
    }
};

/// RESP2 增量解析器：喂入字节流，产出完整回复
class ZENITH_API RespParser {
public:
    enum class State { Incomplete, Ok, Error };
    struct Result {
        State state = State::Incomplete;
        std::string error;
        RedisReply reply;
    };
    Result feed(const uint8_t* data, size_t n);
    Result feed(core::Buffer& buf);

private:
    Result parseValue(const std::string& pending, size_t& pos, int depth);
    static bool takeLine(const std::string& s, size_t& pos, std::string& line);

    std::string pending_;
};

/// 编码 RESP 命令（*N\r\n$len\r\narg...）
ZENITH_API std::string encodeCommand(const std::vector<std::string>& args);

// ---------------------------------------------------------------------------
// 客户端
// ---------------------------------------------------------------------------
class ZENITH_API RedisClient : private core::NonCopyable {
public:
    using ReplyCallback = std::function<void(core::Result<RedisReply>)>;

    explicit RedisClient(net::EventLoop* loop);

    /// 连接（每实例一次）。回调在 loop 线程。
    void connect(const std::string& host, uint16_t port,
                 std::function<void()> onReady,
                 std::function<void(const std::string&)> onFail);

    /// 发送命令（支持流水线：多条命令可连续发送，回调按序触发）。
    void command(std::vector<std::string> args, ReplyCallback cb);

    void get(const std::string& key, ReplyCallback cb);
    void set(const std::string& key, const std::string& value, ReplyCallback cb = nullptr);
    void del(const std::string& key, ReplyCallback cb = nullptr);

    [[nodiscard]] bool connected() const noexcept { return static_cast<bool>(conn_); }

    /// 阻塞便捷形态（独立连接；测试/CLI 用）。
    static core::Result<RedisReply> commandBlocking(const std::string& host, uint16_t port,
                                                    std::vector<std::string> args,
                                                    int timeoutMs = 4000);

private:
    void handleIncoming(core::Buffer& buf);

    net::EventLoop* loop_;
    net::TcpConnection::Ptr conn_;
    RespParser parser_;
    std::deque<ReplyCallback> pending_;
};

} // namespace zenith::redis
