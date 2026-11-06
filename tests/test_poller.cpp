// 通知后端选择 + 基本就绪语义（io_uring / epoll / kqueue / WSAPoll 通用）
#include <cstdio>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/net/poller.hpp"
#include "zenith/net/poller_io_uring.hpp"

using namespace zenith;

ZTEST(Poller, SelectsBackend) {
    auto p = net::createPoller();
    ZEXPECT_TRUE(p != nullptr);
    const char* name = p->name();
    ZEXPECT_TRUE(name != nullptr);
    ZEXPECT_TRUE(std::string(name).size() > 0);
    std::printf("      [poller] active backend: %s\n", name);
}

ZTEST(Poller, IoUringProbeIsConsistent) {
    // 探测结论必须稳定：连续两次一致，且 createIoUringPoller 与之一致
    const bool a = net::ioUringAvailable();
    const bool b = net::ioUringAvailable();
    ZEXPECT_EQ(a, b);
    auto p = net::createIoUringPoller();
    if (a) {
        ZEXPECT_TRUE(p != nullptr);
        if (p != nullptr) ZEXPECT_EQ(std::string(p->name()), std::string("io_uring"));
    } else {
        ZEXPECT_TRUE(p == nullptr);
    }
}

#if !defined(_WIN32)
ZTEST(Poller, ReadableWakeup) {
    int sv[2] = {-1, -1};
    ZEXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

    auto p = net::createPoller();
    ZEXPECT_TRUE(p->registerFd(sv[0], net::kPollReadable));

    // 没有数据：poll 应在超时后返回 0 条
    std::vector<net::PollResult> out;
    const int idle = p->poll(out, 50);
    ZEXPECT_EQ(idle, 0);
    ZEXPECT_TRUE(out.empty());

    // 对端写入 → 本端可读
    ZEXPECT_EQ(::write(sv[1], "x", 1), 1);
    out.clear();
    const int n = p->poll(out, 2000);
    ZEXPECT_TRUE(n >= 1);
    bool found = false;
    for (const auto& r : out) {
        if (r.fd == sv[0] && (r.revents & net::kPollReadable) != 0) found = true;
    }
    ZEXPECT_TRUE(found);

    ZEXPECT_TRUE(p->deregisterFd(sv[0]));
    ::close(sv[0]);
    ::close(sv[1]);
}

ZTEST(Poller, WritableAndRemove) {
    int sv[2] = {-1, -1};
    ZEXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    auto p = net::createPoller();
    ZEXPECT_TRUE(p->registerFd(sv[1], net::kPollWritable));

    std::vector<net::PollResult> out;
    const int n = p->poll(out, 2000);
    ZEXPECT_TRUE(n >= 1);
    bool writable = false;
    for (const auto& r : out) {
        if (r.fd == sv[1] && (r.revents & net::kPollWritable) != 0) writable = true;
    }
    ZEXPECT_TRUE(writable);

    // 注销后不应再收到事件
    ZEXPECT_TRUE(p->deregisterFd(sv[1]));
    out.clear();
    (void)p->poll(out, 50);
    bool still = false;
    for (const auto& r : out) {
        if (r.fd == sv[1]) still = true;
    }
    ZEXPECT_FALSE(still);
    ::close(sv[0]);
    ::close(sv[1]);
}
#else
ZTEST(Poller, SocketpairTestsSkippedOnWindows) {
    // socketpair 不在标准里；Windows 的 WSAPoll 语义由其它端到端用例覆盖
    ZEXPECT_TRUE(true);
}
#endif

ZTEST_MAIN
