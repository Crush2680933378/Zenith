// ============================================================================
//  io_uring 通知后端（Linux，无 liburing 依赖：直接走三个 syscall）
//
//  设计：
//    - 每个 fd 保持一个常驻 IORING_OP_POLL_ADD；有事件后立刻重新挂上
//    - poll() 的超时用 IORING_OP_TIMEOUT 实现（io_uring_enter 本身不带超时）
//    - 环境变量 ZENITH_POLLER=io_uring 启用；不可用（老内核/seccomp 禁用）自动回退 epoll
//
//  为什么默认不开：io_uring 在部分容器/加固内核里被禁用，默认开会让"跑不起来"
//  变成默认行为。显式开启 + 运行时探测 + 回退，三件事都做齐才开始用。
// ============================================================================
#pragma once

#include <memory>

#include "zenith/net/poller.hpp"

namespace zenith::net {

/// 当前内核是否支持 io_uring（探测一次并缓存）
ZENITH_API bool ioUringAvailable();

/// 创建 io_uring 后端；不可用时返回 nullptr（调用方回退 epoll）
ZENITH_API std::unique_ptr<Poller> createIoUringPoller();

}  // namespace zenith::net
