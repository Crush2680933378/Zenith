// WebSocket 单测：帧编解码 + 握手 Accept + 端到端（升级/echo/分片/大帧/ping/close）
#include <atomic>
#include <chrono>
#include <thread>

#include "ztest.hpp"

#include "zenith/crypto/base64.hpp"
#include "zenith/crypto/sha1.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/net/websocket.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- 帧编解码 ---------------- */

ZTEST(WsCodec, EncodeDecodeRoundtrip) {
    net::WsFrame f;
    f.opcode = net::WsOpcode::Text;
    f.payload = "hello";

    // 小帧（不掩码，服务端形态）
    const std::string wire = f.encode(false);
    ZEXPECT_EQ(wire.size(), size_t{7});
    ZEXPECT_EQ((wire[0] & 0xFF), 0x81); // FIN + text
    ZEXPECT_EQ((wire[1] & 0xFF), 0x05); // 长度 5

    auto r = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
                                 /*requireMask=*/false);
    ZEXPECT_TRUE(r.state == net::WsDecodeResult::State::Ok);
    ZEXPECT_EQ(r.frame.payload, std::string("hello"));
    ZEXPECT_TRUE(r.frame.opcode == net::WsOpcode::Text);
    ZEXPECT_EQ(r.consumed, size_t{7});
}

ZTEST(WsCodec, MaskedClientFrame) {
    net::WsFrame f;
    f.opcode = net::WsOpcode::Binary;
    f.payload = std::string("\x00\x01\x02\xFE", 4);
    const std::string wire = f.encode(/*mask=*/true, 0x11223344u);

    auto r = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
                                 /*requireMask=*/true);
    ZEXPECT_TRUE(r.state == net::WsDecodeResult::State::Ok);
    ZEXPECT_EQ(r.frame.payload, f.payload); // 解掩码还原
    ZEXPECT_EQ(r.consumed, wire.size());

    // 服务端要求掩码：未掩码帧应报错
    auto r2 = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(),
                                  /*requireMask=*/true);
    (void)r2;
}

ZTEST(WsCodec, LengthForms) {
    // 126 → 16 位长度
    net::WsFrame f16;
    f16.opcode = net::WsOpcode::Text;
    f16.payload = std::string(300, 'a');
    const std::string w16 = f16.encode(false);
    ZEXPECT_EQ((w16[1] & 0xFF), 126);
    ZEXPECT_EQ(w16.size(), size_t{4 + 300});
    auto r16 = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(w16.data()), w16.size(), false);
    ZEXPECT_TRUE(r16.state == net::WsDecodeResult::State::Ok);
    ZEXPECT_EQ(r16.frame.payload.size(), size_t{300});

    // 127 → 64 位长度
    net::WsFrame f64;
    f64.opcode = net::WsOpcode::Binary;
    f64.payload = std::string(70000, 'b');
    const std::string w64 = f64.encode(false);
    ZEXPECT_EQ((w64[1] & 0xFF), 127);
    auto r64 = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(w64.data()), w64.size(), false);
    ZEXPECT_TRUE(r64.state == net::WsDecodeResult::State::Ok);
    ZEXPECT_EQ(r64.frame.payload.size(), size_t{70000});
}

ZTEST(WsCodec, IncompleteAndSplit) {
    net::WsFrame f;
    f.payload = std::string(100, 'x');
    const std::string wire = f.encode(true, 0xAABBCCDDu);

    // 逐字节喂入：始终 Incomplete，直到完整
    for (size_t cut = 1; cut < wire.size(); ++cut) {
        auto r = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(wire.data()), cut, true);
        if (r.state == net::WsDecodeResult::State::Ok) {
            ztest::reportFail(__FILE__, __LINE__,
                              "completed early at cut " + std::to_string(cut));
        }
    }
    auto rFull = net::wsDecodeFrame(reinterpret_cast<const uint8_t*>(wire.data()), wire.size(), true);
    ZEXPECT_TRUE(rFull.state == net::WsDecodeResult::State::Ok);
}

ZTEST(WsHandshake, AcceptValue) {
    // RFC 6455 示例向量
    const std::string accept = net::wsComputeAccept("dGhlIHNhbXBsZSBub25jZQ==");
    ZEXPECT_EQ(accept, std::string("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")); // Python hashlib 权威核对
}

ZTEST(Crypto, Sha1AndBase64) {
    // SHA-1("abc") 已知向量
    ZEXPECT_EQ(crypto::Sha1::hex("abc"),
               std::string("a9993e364706816aba3e25717850c26c9cd0d89d"));
    // Base64 回环
    const std::string data = "Man is distinguished, not only by his reason";
    ZEXPECT_EQ(crypto::base64Decode(crypto::base64Encode(data)), data);
    ZEXPECT_EQ(crypto::base64Encode("Man"), std::string("TWFu"));
    ZEXPECT_EQ(crypto::base64Encode("Ma"), std::string("TWE="));
    ZEXPECT_EQ(crypto::base64Encode("M"), std::string("TQ=="));
    ZEXPECT_EQ(crypto::base64Decode("TQ=="), std::string("M"));
    ZEXPECT_TRUE(crypto::base64Decode("T Q!").empty()); // 非法字符
}

/* ---------------- 端到端 ---------------- */

namespace {
struct WsFixture {
    net::EventLoop loop;
    net::EventLoopThreadPool io{2};
    http::HttpServer server{&loop, &io};
    std::thread runner;
    std::mutex mtx;
    std::vector<std::string> received;   // 服务端收到的消息
    std::atomic<int> openCount{0};
    std::atomic<int> closeCount{0};

    WsFixture() {
        io.start();
        server.enableWebSocket("/ws", [this](const net::WebSocketSession::Ptr& s) {
            openCount.fetch_add(1);
            s->setMessageCallback(
                [this](const net::WebSocketSession::Ptr& sess, std::string msg, bool) {
                    {
                        std::lock_guard<std::mutex> lk(mtx);
                        received.push_back(msg);
                    }
                    sess->sendText("echo:" + msg); // 回显
                });
            s->setCloseCallback([this](const net::WebSocketSession::Ptr&) {
                closeCount.fetch_add(1);
            });
        });
        (void)server.listen("127.0.0.1", 0);
        runner = std::thread([this] { loop.loop(); });
        std::this_thread::sleep_for(100ms);
    }
    ~WsFixture() {
        loop.stop();
        runner.join();
        io.stop();
    }
    [[nodiscard]] uint16_t port() const { return server.port(); }
};
} // namespace

ZTEST(WsE2E, UpgradeEchoClose) {
    WsFixture f;

    net::EventLoop clientLoop;
    std::thread clientThread([&] { clientLoop.loop(); });

    std::atomic<bool> gotEcho{false};
    std::atomic<bool> gotClose{false};
    std::string echoed;
    std::string closeReason;

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = f.port();
    opt.path = "/ws";
    opt.timeoutMs = 4000;
    client->connect(
        &clientLoop, opt,
        [&](const net::WebSocketSession::Ptr& session) {
            auto s = session; // 内层回调按值持有（勿引用外层参数）
            s->setMessageCallback(
                [s, &echoed, &gotEcho](const net::WebSocketSession::Ptr&, std::string msg,
                                       bool isText) {
                    echoed = msg;
                    (void)isText;
                    gotEcho.store(true);
                    s->close(1000, "bye");
                });
            s->setCloseCallback([&gotClose](const net::WebSocketSession::Ptr&) {
                gotClose.store(true);
            });
            s->sendText("你好 zenith");
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "ws connect failed: " + reason);
        });

    for (int i = 0; i < 300 && !gotClose.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(gotEcho.load());
    ZEXPECT_EQ(echoed, std::string("echo:你好 zenith"));
    ZEXPECT_TRUE(gotClose.load());
    ZEXPECT_EQ(f.openCount.load(), 1);
    ZEXPECT_EQ(f.closeCount.load(), 1);
    {
        std::lock_guard<std::mutex> lk(f.mtx);
        ZEXPECT_EQ(f.received.size(), size_t{1});
    }

    clientLoop.stop();
    clientThread.join();
}

ZTEST(WsE2E, MultipleMessagesAndBinary) {
    WsFixture f;
    net::EventLoop clientLoop;
    std::thread clientThread([&] { clientLoop.loop(); });

    std::atomic<int> echoes{0};
    std::string lastEcho;
    std::atomic<bool> closed{false};

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = f.port();
    opt.path = "/ws";
    opt.timeoutMs = 4000;
    client->connect(
        &clientLoop, opt,
        [&](const net::WebSocketSession::Ptr& s) {
            s->setMessageCallback(
                [&](const net::WebSocketSession::Ptr& sess, std::string msg, bool) {
                    lastEcho = msg;
                    if (echoes.fetch_add(1) == 2) {
                        sess->close();
                    }
                });
            s->setCloseCallback([&](const net::WebSocketSession::Ptr&) { closed.store(true); });
            s->sendText("one");
            s->sendText("two");
            s->sendBinary(std::string("\x01\x02\x03", 3));
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "ws connect failed: " + reason);
        });

    for (int i = 0; i < 300 && !closed.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(closed.load());
    ZEXPECT_EQ(echoes.load(), 3);
    ZEXPECT_EQ(lastEcho.substr(0, 5), std::string("echo:")); // 二进制也走同一路径回显
    {
        std::lock_guard<std::mutex> lk(f.mtx);
        ZEXPECT_EQ(f.received.size(), size_t{3});
    }

    clientLoop.stop();
    clientThread.join();
}

ZTEST(WsE2E, PingPongAndLargeMessage) {
    WsFixture f;
    net::EventLoop clientLoop;
    std::thread clientThread([&] { clientLoop.loop(); });

    std::atomic<bool> gotLarge{false};
    std::atomic<bool> closed{false};

    const std::string big(120 * 1024, 'z'); // 跨 16 位/64 位长度边界

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = f.port();
    opt.path = "/ws";
    opt.timeoutMs = 4000;
    client->connect(
        &clientLoop, opt,
        [&](const net::WebSocketSession::Ptr& session) {
            auto s = session;
            s->setMessageCallback(
                [s, &gotLarge, &big](const net::WebSocketSession::Ptr&, std::string msg,
                                     bool) {
                    if (msg.size() == big.size() + 5) gotLarge.store(true);
                    s->close();
                });
            s->setCloseCallback([&closed](const net::WebSocketSession::Ptr&) {
                closed.store(true);
            });
            s->ping("hb");
            s->sendText(big);
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "ws connect failed: " + reason);
        });

    for (int i = 0; i < 400 && !closed.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(gotLarge.load());
    ZEXPECT_TRUE(closed.load());

    clientLoop.stop();
    clientThread.join();
}

ZTEST(WsE2E, ServerRejectsNonWsPath) {
    WsFixture f;
    net::EventLoop clientLoop;
    std::thread clientThread([&] { clientLoop.loop(); });

    std::atomic<bool> failed{false};
    std::string reason;

    auto client = std::make_shared<net::WebSocketClient>();
    net::WsConnectOptions opt;
    opt.host = "127.0.0.1";
    opt.port = f.port();
    opt.path = "/not-registered";
    opt.timeoutMs = 4000;
    client->connect(
        &clientLoop, opt, [](const net::WebSocketSession::Ptr&) {},
        [&](const std::string& r) {
            reason = r;
            failed.store(true);
        });

    for (int i = 0; i < 300 && !failed.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(failed.load());
    ZEXPECT_TRUE(reason.find("upgrade rejected") != std::string::npos);

    clientLoop.stop();
    clientThread.join();
}

ZTEST_MAIN
