// zenith::net 单测：EventLoop（任务/定时器/唤醒）+ TCP echo 端到端
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <thread>

#include "ztest.hpp"

#include "zenith/net/event_loop_thread_pool.hpp"
#include "zenith/net/tcp_client.hpp"
#include "zenith/net/tcp_server.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- EventLoop 基础 ---------------- */

ZTEST(EventLoop, ScheduleAndCancel) {
    net::EventLoop loop;
    std::thread runner([&] { loop.loop(); });

    std::atomic<int> fired{0};
    const uint64_t t1 = loop.scheduleAfter(30ms, [&] { fired.fetch_add(1); });
    loop.scheduleAfter(50ms, [&] { fired.fetch_add(1); });
    ZEXPECT_TRUE(loop.cancel(t1)); // 取消第一个

    std::this_thread::sleep_for(200ms);
    loop.stop();
    runner.join();
    ZEXPECT_EQ(fired.load(), 1); // 只有第二个触发
}

ZTEST(EventLoop, RunInLoopFromOtherThread) {
    net::EventLoop loop;
    std::thread runner([&] { loop.loop(); });
    std::this_thread::sleep_for(100ms); // 等 loop() 启动并绑定线程身份

    std::atomic<bool> ran{false};
    std::thread::id runOn{};
    std::promise<void> done;
    loop.runInLoop([&] {
        runOn = std::this_thread::get_id();
        ran.store(true);
        done.set_value();
    });
    ZEXPECT_TRUE(done.get_future().wait_for(2s) == std::future_status::ready);
    ZEXPECT_TRUE(ran.load());
    ZEXPECT_EQ(runOn, runner.get_id()); // 在 loop 线程执行
    loop.stop();
    runner.join();
}

ZTEST(EventLoop, PollerNameAndThreadSafetyOfStop) {
    net::EventLoop loop; // 未运行也可 stop
    loop.stop();
    ZEXPECT_TRUE(std::string(loop.pollerName()).size() > 0);
}

ZTEST(EventLoopThreadPool, RoundRobin) {
    net::EventLoopThreadPool pool(3);
    pool.start();
    std::this_thread::sleep_for(50ms); // 等 loop 线程起来
    net::EventLoop* a = pool.nextLoop();
    net::EventLoop* b = pool.nextLoop();
    net::EventLoop* c = pool.nextLoop();
    net::EventLoop* d = pool.nextLoop();
    ZEXPECT_TRUE(a != nullptr && b != nullptr && c != nullptr);
    ZEXPECT_TRUE(a != b);
    ZEXPECT_TRUE(b != c);
    ZEXPECT_EQ(a, d); // 循环
    pool.stop();
}

/* ---------------- TCP echo 端到端 ---------------- */

ZTEST(Tcp, EchoServerRoundtrip) {
    net::EventLoop serverLoop;
    net::EventLoopThreadPool ioPool(2);
    ioPool.start();

    net::TcpServer server(&serverLoop, &ioPool);
    std::atomic<int> receivedBytes{0};
    std::string accumulated;
    std::mutex accMtx;

    server.setMessageCallback([&](const net::TcpConnection::Ptr& conn, core::Buffer& buf) {
        const std::string data = buf.readAllAsString();
        {
            std::lock_guard<std::mutex> lk(accMtx);
            accumulated += data;
        }
        receivedBytes.fetch_add(static_cast<int>(data.size()));
        conn->send(data); // echo
    });

    auto lr = server.listen("127.0.0.1", 0);
    ZEXPECT_TRUE(static_cast<bool>(lr));
    const uint16_t port = server.port();
    ZEXPECT_TRUE(port > 0);

    std::thread serverThread([&] { serverLoop.loop(); });
    std::this_thread::sleep_for(100ms); // 等监听生效

    // 客户端：独立 loop + TcpClient
    net::EventLoop clientLoop;
    std::thread clientThread([&] { clientLoop.loop(); });

    std::string echoed;
    std::atomic<bool> gotEcho{false};
    auto client = std::make_shared<net::TcpClient>(&clientLoop);
    net::ConnectOptions copt;
    copt.host = "127.0.0.1";
    copt.port = port;
    copt.timeoutMs = 3000;
    client->connect(
        copt,
        [&](const net::TcpConnection::Ptr& conn) {
            conn->setMessageCallback([&](const net::TcpConnection::Ptr&, core::Buffer& buf) {
                echoed += buf.readAllAsString();
                if (echoed.size() >= 12) gotEcho.store(true);
            });
            conn->send("hello ");
            conn->send("zenith!"); // 分两段发送
        },
        [](const std::string& reason) {
            ztest::reportFail(__FILE__, __LINE__, "connect failed: " + reason);
        });

    for (int i = 0; i < 200 && !gotEcho.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    ZEXPECT_TRUE(gotEcho.load());
    ZEXPECT_EQ(echoed, std::string("hello zenith!"));

    clientLoop.stop();
    clientThread.join();
    serverLoop.stop();
    serverThread.join();
    ioPool.stop();
    {
        std::lock_guard<std::mutex> lk(accMtx);
        ZEXPECT_EQ(accumulated, std::string("hello zenith!"));
    }
}

ZTEST(Tcp, ConnectRefusedFails) {
    net::EventLoop loop;
    std::thread runner([&] { loop.loop(); });

    std::atomic<bool> failed{false};
    std::string reason;
    auto client = std::make_shared<net::TcpClient>(&loop);
    net::ConnectOptions copt;
    copt.host = "127.0.0.1";
    copt.port = 1; // 几乎不可能有服务
    copt.timeoutMs = 1500;
    client->connect(copt, [](const net::TcpConnection::Ptr&) {},
                    [&](const std::string& r) {
                        reason = r;
                        failed.store(true);
                    });

    for (int i = 0; i < 300 && !failed.load(); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    loop.stop();
    runner.join();
    ZEXPECT_TRUE(failed.load());
    ZEXPECT_TRUE(!reason.empty());
}

ZTEST_MAIN
