// ============================================================================
//  zenith::net::WorkerSupervisor —— 多进程 worker（M5.6b，仅 POSIX/Linux）
//
//  形态：主进程 fork N 个 worker；每个 worker 独立 EventLoop + HttpServer，
//  通过 SO_REUSEPORT 共享同一监听端口（内核级 accept 均衡）。
//  主进程职责：waitpid 监视 worker，异常退出自动补齐；SIGTERM 优雅全停。
//
//  平台：Linux/macOS（fork + SO_REUSEPORT）。Windows 不支持 → start() 返回 Err。
//  编译门控：CMake 仅在 UNIX 编入（CI linux 矩阵验证编译与用例）。
//
//  用法：
//    net::WorkerSupervisor sup;
//    sup.run(4, "0.0.0.0", 8080, [](net::EventLoop& loop, int workerId) {
//        // 在此建 HttpServer/路由；listen 由 run 内部完成（REUSEPORT）
//    });
// ============================================================================
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

namespace zenith::net {

class EventLoop;

struct ZENITH_API WorkerStats {
    int desired = 0;        // 目标 worker 数
    int alive = 0;          // 存活数
    uint64_t spawns = 0;    // 累计派生次数
    uint64_t exits = 0;     // 累计退出次数
};

class ZENITH_API WorkerSupervisor {
public:
    /// workerSetup：每个 worker 子进程内调用（搭建服务；listen 由框架完成）
    using SetupCallback = std::function<void(EventLoop& loop, int workerId)>;

    WorkerSupervisor() = default;
    ~WorkerSupervisor() { shutdown(); }

    WorkerSupervisor(const WorkerSupervisor&) = delete;
    WorkerSupervisor& operator=(const WorkerSupervisor&) = delete;

    /// 阻塞运行主循环（SIGTERM/SIGINT 退出）。仅 POSIX。
    /// workers=0 → 取 hardware_concurrency。
    core::Result<void> run(int workers, const std::string& host, uint16_t port,
                           SetupCallback workerSetup, int backlog = 128);

    /// 请求全部 worker 优雅退出（SIGTERM → 等待 → SIGKILL 兜底）
    void shutdown();

    [[nodiscard]] WorkerStats stats() const noexcept { return stats_; }

private:
    int spawnWorker(const std::string& host, uint16_t port, int backlog, int workerId);
    void reapAndRespawn(bool block);

    SetupCallback setup_;
    WorkerStats stats_;
    std::vector<int64_t> pids_; // 仅主进程访问（POSIX pid 存 int64）
    bool running_ = false;
};

} // namespace zenith::net
