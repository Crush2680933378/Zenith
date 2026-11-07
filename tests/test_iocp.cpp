// IOCP 传输层：监听 / 接入 / 收发（Windows 原生；其它平台验证 supported()==false）
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/net/iocp.hpp"

using namespace zenith;
using namespace std::chrono_literals;

namespace {
/// 阻塞式客户端：连上后发一条消息，读回等长内容（带超时）
bool echoOnce(uint16_t port, const std::string& msg, std::string& out, int timeoutMs = 2000) {
#if defined(_WIN32)
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int fd = static_cast<int>(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (fd < 0) return false;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0) {
#if defined(_WIN32)
        closesocket(fd);
#else
        ::close(fd);
#endif
        return false;
    }
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
    bool ok = false;
    if (::send(fd, msg.data(), static_cast<int>(msg.size()), 0) == static_cast<int>(msg.size())) {
        out.assign(msg.size(), '\0');
        size_t got = 0;
        while (got < msg.size()) {
            const int n = ::recv(fd, &out[got], static_cast<int>(msg.size() - got), 0);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        ok = got == msg.size();
    }
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
    return ok;
}
}  // namespace

ZTEST(Iocp, SupportedMatchesPlatform) {
#if defined(_WIN32)
    ZEXPECT_TRUE(net::IocpServer::supported());
#else
    ZEXPECT_FALSE(net::IocpServer::supported());
#endif
}

#if defined(_WIN32)
ZTEST(IocpE2E, EchoThreeClients) {
    net::IocpServer srv;
    ZEXPECT_TRUE(srv.listen("127.0.0.1", 0));
    ZEXPECT_TRUE(srv.port() != 0);

    std::atomic<int> closes{0};
    ZEXPECT_TRUE(srv.start(
        [&srv](uint64_t id, const char* data, size_t n) {
            srv.send(id, std::string(data, n));  // 回显
        },
        [&closes](uint64_t) { closes.fetch_add(1); }));

    const uint16_t port = srv.port();
    std::atomic<int> okCount{0};
    std::vector<std::thread> clients;
    for (int i = 0; i < 3; ++i) {
        clients.emplace_back([port, i, &okCount] {
            const std::string msg = "hello-" + std::to_string(i);
            std::string echoed;
            if (echoOnce(port, msg, echoed) && echoed == msg) okCount.fetch_add(1);
        });
    }
    for (auto& t : clients)
        t.join();
    ZEXPECT_EQ(okCount.load(), 3);

    // 等连接数回落（客户端已关闭）
    for (int i = 0; i < 200 && srv.connectionCount() > 0; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    srv.stop();
    ZEXPECT_TRUE(closes.load() >= 1);
}

ZTEST(IocpE2E, SendToUnknownConnectionFails) {
    net::IocpServer srv;
    ZEXPECT_TRUE(srv.listen("127.0.0.1", 0));
    ZEXPECT_TRUE(srv.start([](uint64_t, const char*, size_t) {}, [](uint64_t) {}));
    ZEXPECT_FALSE(srv.send(9999, "x"));  // 不存在的连接
    srv.stop();
}
#else
ZTEST(IocpE2E, SkippedOnNonWindows) {
    ZEXPECT_TRUE(true);
}
#endif

ZTEST_MAIN
