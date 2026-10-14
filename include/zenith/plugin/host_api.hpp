// ============================================================================
//  HostApi —— 把宿主 C++ 能力（注册表/线程池/配置/日志）翻译成
//  zenith_host_api C 函数指针表的适配层。PluginManager 为每个插件生成
//  一张表（携带该插件的 owner 令牌）。
// ============================================================================
#pragma once

#include "zenith/abi/zenith_abi.h"
#include "zenith/core/export.hpp"

namespace zenith::concurrency {
class ThreadPool;
}

namespace zenith::core {
class KeyValueConfig;
}

namespace zenith::plugin {

class ServiceRegistry;

struct HostContext {
    ServiceRegistry*          registry = nullptr; // 可空：则注册/查询返回 NOT_INITIALIZED
    concurrency::ThreadPool*  pool     = nullptr; // 可空：同上
    core::KeyValueConfig*     config   = nullptr; // 可空：同上
};

/// 构建注入给插件的 C 能力表。ownerToken 会写入 plugin_token 字段。
ZENITH_API zenith_host_api makeHostApiV1(const HostContext& ctx, void* ownerToken);

} // namespace zenith::plugin
