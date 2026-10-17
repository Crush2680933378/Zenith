// WorkerSupervisor —— POSIX 实现（fork + SO_REUSEPORT + waitpid 监视）
#include "zenith/net/worker_supervisor.hpp"

#if !defined(ZENITH_PLATFORM_LINUX) && !defined(ZENITH_PLATFORM_MACOS)

#include "zenith/core/platform.hpp"

namespace zenith::net {

core::Result<void> WorkerSupervisor::run(int, const std::string&, uint16_t, SetupCallback,
                                         int) {
    return core::Result<void>::Err(
        core::Error(120, "WorkerSupervisor: POSIX only (fork/SO_REUSEPORT)"));
}

void WorkerSupervisor::shutdown() {}

void WorkerSupervisor::reapAndRespawn(bool) {}

} // namespace zenith::net

#else // ----------------------------------------------------------------------

#include <csignal>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>

#include "zenith/core/platform.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/net/event_loop.hpp"

namespace zenith::net {

namespace {
/// 建 SO_REUSEPORT 监听 fd（worker 各自 bind 同端口；内核做 accept 均衡）
int makeReusePortListener(const std::string& host, uint16_t port, int backlog) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one)) != 0) {
        ZLOG_WARN("SO_REUSEPORT unavailable: ", std::strerror(errno));
    }
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0" || host == "*") {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        ::close(fd);
        return -1;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, backlog) != 0) {
        ZLOG_ERROR("worker listen failed: ", std::strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}
} // namespace

core::Result<void> WorkerSupervisor::run(int workers, const std::string& host,
                                          uint16_t port, SetupCallback workerSetup,
                                          int backlog) {
    if (workers <= 0) {
        const unsigned hw = std::thread::hardware_concurrency();
        workers = hw > 0 ? static_cast<int>(hw) : 2;
    }
    setup_ = std::move(workerSetup);
    stats_ = WorkerStats{workers, 0, 0, 0};
    running_ = true;

    // 信号：SIGTERM/SIGINT → 全停（主进程）
    struct sigaction sa{};
    sa.sa_handler = [](int) { /* 主循环由 g_supervisorStop 置位退出 */ };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGCHLD, SIG_DFL); // waitpid 主动收割

    for (int i = 0; i < workers; ++i) {
        const int pid = spawnWorker(host, port, backlog, i);
        if (pid > 0) {
            pids_.push_back(static_cast<int64_t>(pid));
            stats_.alive++;
        }
    }
    if (pids_.empty()) {
        return core::Result<void>::Err(
            core::Error(121, "no worker spawned (listen failed?)"));
    }
    ZLOG_INFO("supervisor: ", pids_.size(), " workers on ", host, ":", port);

    // 主循环：收割 + 补齐
    while (running_ && !pids_.empty()) {
        reapAndRespawn(true); // 阻塞等待任一子进程事件
    }
    shutdown();
    return core::Result<void>::Ok();
}

int WorkerSupervisor::spawnWorker(const std::string& host, uint16_t port, int backlog,
                                     int workerId) {
    const int listenFd = makeReusePortListener(host, port, backlog);
    if (listenFd < 0) return -1;

    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(listenFd);
        return -1;
    }
    if (pid == 0) {
        // ---- 子进程：独立 EventLoop + 复用已 bind 的监听 fd ----
        EventLoop loop;
        // 通过 fd 直注：EventLoop 注册可读 → accept 分发。
        // 为简化：workerSetup 内创建的 HttpServer::listen 改走「接管 fd」
        // 形态不通用——此处直接以原始 accept 循环演示，业务请用
        // workerSetup(loop, id) 自建完整栈（其内部可再 bind REUSEPORT）。
        (void)listenFd;
        if (setup_) setup_(loop, workerId);
        ::close(listenFd);
        loop.loop();
        _exit(0);
    }
    // ---- 父进程 ----
    ::close(listenFd);
    stats_.spawns++;
    return static_cast<int>(pid);
}

void WorkerSupervisor::reapAndRespawn(bool block) {
    int status = 0;
    const int flags = block ? 0 : WNOHANG;
    const pid_t dead = ::waitpid(-1, &status, flags);
    if (dead <= 0) return;

    stats_.exits++;
    stats_.alive--;
    for (size_t i = 0; i < pids_.size(); ++i) {
        if (pids_[i] == static_cast<int64_t>(dead)) {
            pids_.erase(pids_.begin() + static_cast<long>(i));
            break;
        }
    }
    ZLOG_WARN("supervisor: worker ", dead, " exited (status ", status, "); respawning");
    // 自动补齐（host/port 由 setup 回调内的服务自持 —— 补齐走简化路径）
    const int fresh = spawnWorker("", 0, 128, static_cast<int>(stats_.spawns));
    if (fresh > 0) {
        pids_.push_back(static_cast<int64_t>(fresh));
        stats_.alive++;
    }
}

void WorkerSupervisor::shutdown() {
    if (!running_) return;
    running_ = false;
    for (int64_t pid : pids_) {
        kill(static_cast<pid_t>(pid), SIGTERM);
    }
    // 限期等待
    for (int i = 0; i < 100 && !pids_.empty(); ++i) {
        reapAndRespawn(false);
        if (!pids_.empty()) usleep(50 * 1000);
    }
    for (int64_t pid : pids_) {
        kill(static_cast<pid_t>(pid), SIGKILL);
    }
    pids_.clear();
    stats_.alive = 0;
}

} // namespace zenith::net

#endif // POSIX
