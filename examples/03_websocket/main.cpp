// ============================================================================
//  03_websocket —— WebSocket 广播 + REST 计数器示例
//   终端1: ./ws_demo                    （监听 8081）
//   终端2: curl http://127.0.0.1:8081/api/stats
//   浏览器控制台:
//     const ws = new WebSocket("ws://127.0.0.1:8081/ws");
//     ws.onmessage = e => console.log(e.data); ws.send("hello");
// ============================================================================
#include <atomic>
#include <chrono>
#include <set>
#include <thread>

#include "zenith/http/http_server.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/net/websocket.hpp"

int main() {
    using namespace zenith;

    auto& facade = log::LoggerFacade::instance();
    facade.setLevel(log::LogLevel::Debug);
    facade.addSink(std::make_shared<log::ConsoleLogger>(true, true));

    net::EventLoop loop;
    net::EventLoopThreadPool io(2);
    io.start();
    http::HttpServer server(&loop, &io);

    // 会话登记（广播用；IO 线程访问，M3 示例单线程 IO 足够，简单互斥保护）
    std::mutex sessionsMtx;
    std::set<net::WebSocketSession::Ptr> sessions;
    std::atomic<int> totalMessages{0};

    server.enableWebSocket("/ws", [&](const net::WebSocketSession::Ptr& s) {
        {
            std::lock_guard<std::mutex> lk(sessionsMtx);
            sessions.insert(s);
        }
        ZLOG_INFO("ws open: ", s->peer(), " (online=", sessions.size(), ")");
        s->setMessageCallback(
            [&](const net::WebSocketSession::Ptr& sess, std::string msg, bool isText) {
                totalMessages.fetch_add(1);
                // 广播给所有在线会话
                std::lock_guard<std::mutex> lk(sessionsMtx);
                for (const auto& other : sessions) {
                    if (isText) {
                        other->sendText("[" + sess->peer() + "] " + msg);
                    } else {
                        other->sendBinary(msg);
                    }
                }
            });
        s->setCloseCallback([&](const net::WebSocketSession::Ptr& sess) {
            std::lock_guard<std::mutex> lk(sessionsMtx);
            sessions.erase(sess);
            ZLOG_INFO("ws close: ", sess->peer(), " (online=", sessions.size(), ")");
        });
        s->sendText("welcome to zenith ws");
    });

    // REST：状态查询
    server.router().get("/api/stats", [&](http::Context& c) {
        std::lock_guard<std::mutex> lk(sessionsMtx);
        http::json::JsonValue v = http::json::JsonValue::object();
        v["online"] = static_cast<int64_t>(sessions.size());
        v["total_messages"] = static_cast<int64_t>(totalMessages.load());
        c.json(200, v);
    });

    auto lr = server.listen("0.0.0.0", 8081);
    if (!lr) {
        ZLOG_ERROR("listen failed: ", lr.error().message);
        return 1;
    }
    ZLOG_INFO("Zenith ws demo on http://127.0.0.1:8081 (ws://127.0.0.1:8081/ws)");

    loop.loop();
    io.stop();
    facade.flush();
    facade.shutdown();
    return 0;
}
