// net 层内部工具（不在公共头暴露 winsock/unistd 细节）
#pragma once

#include "zenith/net/socket.hpp"

namespace zenith::net::detail {

/// 自连接 UDP socket：跨平台「唤醒管道」（Windows 无 pipe()）。
/// 返回 kInvalidFd 表示失败（EventLoop 退化为 1s 轮询上限兜底）。
fd_t createWakeupFd();

void closeRaw(fd_t fd);
int rawRead(fd_t fd, void* buf, size_t n);          // >0 / 0 / -1
int rawWrite(fd_t fd, const void* buf, size_t n);   // >0 / -1
void setFdNonBlocking(fd_t fd, bool on);
void shutdownWrite(fd_t fd);                        // shutdown(fd, SHUT_WR) 半关闭
int getSocketError(fd_t fd);                        // getsockopt SO_ERROR；-1 表示查询失败

} // namespace zenith::net::detail
