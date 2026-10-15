// ============================================================================
//  zenith::net 信号处理（M5.1 优雅停机配套）
//  installSignalHandler(cb)：SIGINT/SIGTERM（Windows: CTRL_C/CTRL_BREAK/
//  CTRL_CLOSE）→ 独立线程触发回调（回调内可安全做阻塞操作，如停 EventLoop）
// ============================================================================
#pragma once

#include <functional>

#include "zenith/core/export.hpp"

namespace zenith::net {

/// 安装一次性处理器（幂等：重复安装覆盖前次）。返回是否成功。
ZENITH_API bool installSignalHandler(std::function<void()> handler);

} // namespace zenith::net
