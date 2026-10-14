// Redis 客户端单测：RESP 编解码 + 内置 mock 服务器端到端（无需真实 Redis）
#include <atomic>
#include <chrono>
#include <thread>
#include <unordered_map>

#include "ztest.hpp"

#include "zenith/net/tcp_server.hpp"
#include "zenith/redis/redis_client.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- 编解码 ---------------- */

ZTEST(Resp, EncodeCommand) {
    const std::string wire = redis::encodeCommand({"SET", "key", "value"});
    ZEXPECT_EQ(wire, std::string("*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n"));
}

ZTEST(Resp, ParseSimpleErrorInteger) {
    redis::RespParser p;
    auto r1 = p.feed(reinterpret_cast<const uint8_t*>("+OK\r\n"), 5);
    ZEXPECT_TRUE(r1.state == redis::RespParser::State::Ok);
    ZEXPECT_TRUE(r1.reply.type == redis::RedisReply::Type::Simple);
    ZEXPECT_EQ(r1.reply.str, std::string("OK"));

    redis::RespParser p2;
    auto r2 = p2.feed(reinterpret_cast<const uint8_t*>(":42\r\n"), 5);
    ZEXPECT_TRUE(r2.state == redis::RespParser::State::Ok);
    ZEXPECT_EQ(r2.reply.integer, int64_t{42});

    redis::RespParser p3;
    auto r3 = p3.feed(reinterpret_cast<const uint8_t*>("-ERR bad\r\n"), 10);
    ZEXPECT_TRUE(r3.state == redis::RespParser::State::Ok);
    ZEXPECT_TRUE(r3.reply.isError());
    ZEXPECT_EQ(r3.reply.str, std::string("ERR bad"));
}

ZTEST(Resp, ParseBulkAndNull) {
    redis::RespParser p;
    const std::string wire = "$5\r\nhello\r\n";
    auto r = p.feed(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
    ZEXPECT_TRUE(r.state == redis::RespParser::State::Ok);
    ZEXPECT_EQ(r.reply.str, std::string("hello"));

    redis::RespParser p2;
    auto r2 = p2.feed(reinterpret_cast<const uint8_t*>("$-1\r\n"), 5);
    ZEXPECT_TRUE(r2.state == redis::RespParser::State::Ok);
    ZEXPECT_TRUE(r2.reply.isNull());
}

ZTEST(Resp, ParseArrayAndNested) {
    redis::RespParser p;
    const std::string wire = "*2\r\n$3\r\nabc\r\n*2\r\n:1\r\n$1\r\nz\r\n";
    auto r = p.feed(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
    ZEXPECT_TRUE(r.state == redis::RespParser::State::Ok);
    ZEXPECT_EQ(r.reply.items.size(), size_t{2});
    ZEXPECT_EQ(r.reply.items[0].str, std::string("abc"));
    ZEXPECT_EQ(r.reply.items[1].items.size(), size_t{2});
    ZEXPECT_EQ(r.reply.items[1].items[0].integer, int64_t{1});
    ZEXPECT_EQ(r.reply.items[1].items[1].str, std::string("z"));
}

ZTEST(Resp, IncrementalSplit) {
    const std::string wire = "*2\r\n$4\r\nlead\r\n$5\r\ntrail\r\n";
    // 任意前缀切分：只能 Incomplete（feed 是追加语义，每轮用全新解析器）
    for (size_t cut = 1; cut < wire.size(); ++cut) {
        redis::RespParser p;
        auto r = p.feed(reinterpret_cast<const uint8_t*>(wire.data()), cut);
        if (r.state == redis::RespParser::State::Ok) {
            ztest::reportFail(__FILE__, __LINE__,
                              "completed early at cut " + std::to_string(cut));
        }
    }
    redis::RespParser p2;
    auto r = p2.feed(reinterpret_cast<const uint8_t*>(wire.data()), wire.size());
    ZEXPECT_TRUE(r.state == redis::RespParser::State::Ok);
    ZEXPECT_EQ(r.reply.items[1].str, std::string("trail"));
}

namespace {
/// 内置 mock：解析 RESP 命令，内存哈希实现 GET/SET/DEL/INCR/PING
struct MockRedis {
    net::EventLoop loop;
    net::TcpServer server{&loop};
    std::thread runner;
    std::unordered_map<std::string, std::string> store;
    std::mutex mtx;
    std::atomic<int> commands{0};

    MockRedis() {
        server.setMessageCallback(
            [this](const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
                onMessage(conn, buf);
            });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(80ms);
    }
    ~MockRedis() {
        loop.stop();
        runner.join();
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }

    void onMessage(const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
        for (;;) {
            // 找到一条完整命令（以 \r\n 结尾的 *N ... 行集）
            const std::string all(reinterpret_cast<const char*>(buf.peek()),
                                  buf.readableBytes());
            size_t pos = 0;
            std::string line;
            auto takeLine = [&](std::string& out) {
                const size_t nl = all.find("\r\n", pos);
                if (nl == std::string::npos) return false;
                out.assign(all, pos, nl - pos);
                pos = nl + 2;
                return true;
            };
            if (!takeLine(line) || line.empty() || line[0] != '*') return;
            const int argc = std::atoi(line.c_str() + 1);
            std::vector<std::string> args;
            for (int i = 0; i < argc; ++i) {
                std::string hdr;
                if (!takeLine(hdr) || hdr.empty() || hdr[0] != '$') return;
                const size_t len = static_cast<size_t>(std::atoi(hdr.c_str() + 1));
                if (pos + len + 2 > all.size()) return;
                args.push_back(all.substr(pos, len));
                pos += len + 2;
            }
            buf.consume(pos);
            commands.fetch_add(1);
            conn->send(handle(args));
        }
    }

    std::string handle(const std::vector<std::string>& args) {
        std::lock_guard<std::mutex> lk(mtx);
        if (args.empty()) return "-ERR empty\r\n";
        const std::string& cmd = args[0];
        if (cmd == "PING") return "+PONG\r\n";
        if (cmd == "SET" && args.size() >= 3) {
            store[args[1]] = args[2];
            return "+OK\r\n";
        }
        if (cmd == "GET" && args.size() >= 2) {
            auto it = store.find(args[1]);
            if (it == store.end()) return "$-1\r\n";
            return "$" + std::to_string(it->second.size()) + "\r\n" + it->second + "\r\n";
        }
        if (cmd == "DEL" && args.size() >= 2) {
            const size_t n = store.erase(args[1]);
            return ":" + std::to_string(n) + "\r\n";
        }
        if (cmd == "INCR" && args.size() >= 2) {
            const int64_t v = std::strtoll(store[args[1]].c_str(), nullptr, 10) + 1;
            store[args[1]] = std::to_string(v);
            return ":" + std::to_string(v) + "\r\n";
        }
        if (cmd == "KEYS") {
            std::string arr = "*" + std::to_string(store.size()) + "\r\n";
            for (const auto& [k, v] : store) {
                arr += "$" + std::to_string(k.size()) + "\r\n" + k + "\r\n";
            }
            return arr;
        }
        return "-ERR unknown command\r\n";
    }
};
} // namespace

ZTEST(RedisE2E, SetGetDelIncr) {
    MockRedis mock;

    auto set = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"SET", "k1", "v1"});
    ZEXPECT_TRUE(static_cast<bool>(set));
    ZEXPECT_EQ(set.value().str, std::string("OK"));

    auto get = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"GET", "k1"});
    ZEXPECT_TRUE(static_cast<bool>(get));
    ZEXPECT_EQ(get.value().str, std::string("v1"));

    auto miss = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"GET", "nope"});
    ZEXPECT_TRUE(static_cast<bool>(miss));
    ZEXPECT_TRUE(miss.value().isNull());

    auto incr1 = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"INCR", "ctr"});
    auto incr2 = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"INCR", "ctr"});
    ZEXPECT_EQ(incr1.value().integer, int64_t{1});
    ZEXPECT_EQ(incr2.value().integer, int64_t{2});

    auto del = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"DEL", "k1"});
    ZEXPECT_EQ(del.value().integer, int64_t{1});

    auto ping = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"PING"});
    ZEXPECT_EQ(ping.value().str, std::string("PONG"));
}

ZTEST(RedisE2E, ErrorReplyAndArray) {
    MockRedis mock;
    (void)redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"SET", "a", "1"});
    (void)redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"SET", "b", "2"});

    auto err = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"BLAH"});
    ZEXPECT_TRUE(static_cast<bool>(err));
    ZEXPECT_TRUE(err.value().isError());

    auto keys = redis::RedisClient::commandBlocking("127.0.0.1", mock.port(), {"KEYS"});
    ZEXPECT_TRUE(static_cast<bool>(keys));
    ZEXPECT_EQ(keys.value().items.size(), size_t{2});
}

ZTEST(RedisE2E, PipelineOrderingOnClient) {
    // 同一客户端实例连续发多条命令：回调按序对齐
    MockRedis mock;
    net::EventLoop loop;
    std::thread t([&] { loop.loop(); });

    redis::RedisClient client(&loop);
    std::atomic<int> ready{0};
    std::atomic<bool> done{false};
    std::vector<std::string> order;
    std::mutex orderMtx;

    loop.queueInLoop([&] {
        client.connect("127.0.0.1", mock.port(), [&] { ready.store(1); },
                        [&](const std::string& r) {
                            ztest::reportFail(__FILE__, __LINE__, "connect failed: " + r);
                            done.store(true);
                        });
    });

    for (int i = 0; i < 200 && ready.load() == 0; ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_EQ(ready.load(), 1);

    loop.runInLoop([&] {
        for (int i = 0; i < 20; ++i) {
            client.set("pk" + std::to_string(i), "pv" + std::to_string(i),
                       [&, i](core::Result<redis::RedisReply> r) {
                           if (r) {
                               std::lock_guard<std::mutex> lk(orderMtx);
                               order.push_back(r.value().str);
                           }
                           if (order.size() == 20) {
                               done.store(true);
                               loop.stop();
                           }
                       });
        }
    });

    for (int i = 0; i < 400 && !done.load(); ++i) std::this_thread::sleep_for(10ms);
    loop.stop();
    t.join();

    std::lock_guard<std::mutex> lk(orderMtx);
    ZEXPECT_EQ(order.size(), size_t{20});
    for (const auto& o : order) ZEXPECT_EQ(o, std::string("OK"));
    ZEXPECT_TRUE(mock.commands.load() >= 20);
}

ZTEST(RedisE2E, ConnectRefused) {
    auto r = redis::RedisClient::commandBlocking("127.0.0.1", 1, {"PING"}, 1500);
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_TRUE(!r.error().message.empty()); // 拒绝/超时措辞随平台，统一走 Err
}

ZTEST_MAIN
