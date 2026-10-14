// ============================================================================
//  ServiceRegistry —— 按接口名 + 版本的服务注册表
//
//  两类条目：
//    1. typed：宿主侧 C++ 服务，shared_ptr 托管（如 ILogger、线程池包装）
//    2. raw  ：插件经 C ABI 注册的裸指针 + 可选 destroy 回调 + owner 令牌。
//              宿主在卸载插件动态库之前按 owner 清理并调用 destroy，
//              保证 destroy 执行时代码仍处于加载状态。
//
//  查询语义：query(name, minVersion) —— 版本低于 minVersion 视为不匹配，
//  返回 ZENITH_ERR_VERSION_MISMATCH；不存在返回 ZENITH_ERR_NOT_FOUND。
// ============================================================================
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "zenith/abi/zenith_abi.h"
#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/plugin/service.hpp"

namespace zenith::plugin {

class ZENITH_API ServiceRegistry : private core::NonCopyable {
public:
    ServiceRegistry() = default;
    ~ServiceRegistry();

    /* ---- typed（宿主侧 shared_ptr 服务） ---- */

    template <typename T>
    zenith_status put(std::shared_ptr<T> service, bool replace = false) {
        static_assert(std::is_base_of_v<IService, T>,
                      "typed 服务需继承 zenith::plugin::IService");
        return registerTyped(T::service_name(), T::service_version(),
                             std::static_pointer_cast<void>(std::move(service)), replace);
    }

    /// 查询 typed 服务；未注册或版本不足返回空 shared_ptr。
    template <typename T>
    [[nodiscard]] std::shared_ptr<T> get(uint32_t minVersion = T::service_version()) const {
        static_assert(std::is_base_of_v<IService, T>,
                      "typed 服务需继承 zenith::plugin::IService");
        return std::static_pointer_cast<T>(
            queryTyped(T::service_name(), minVersion));
    }

    /* ---- raw（插件经 C ABI 注册） ---- */

    zenith_status registerRaw(const char* interfaceName, uint32_t implVersion,
                              void* instance, zenith_destroy_fn destroy, void* ownerToken,
                              bool replace = false);

    /// 返回裸指针（不转移所有权；生命周期由注册方/注册表管理）。
    [[nodiscard]] void* queryRaw(const char* interfaceName, uint32_t minVersion,
                                 zenith_status* status = nullptr) const;

    /// 清理某 owner 注册的全部 raw 服务（调用 destroy 后移除），返回清理条数。
    size_t removeByOwner(void* ownerToken);

    /// 清空全部条目（typed 析构 + raw destroy）。宿主停机时调用。
    void clear();

    [[nodiscard]] size_t size() const;
    [[nodiscard]] std::vector<std::pair<std::string, uint32_t>> list() const;

private:
    struct Entry {
        uint32_t             version = 0;
        std::shared_ptr<void> typed;                    // typed 条目非空
        void*                rawInstance = nullptr;     // raw 条目非空
        zenith_destroy_fn    rawDestroy  = nullptr;
        void*                owner       = nullptr;
    };

    zenith_status registerTyped(const char* interfaceName, uint32_t implVersion,
                                std::shared_ptr<void> service, bool replace);
    [[nodiscard]] std::shared_ptr<void> queryTyped(const char* interfaceName,
                                                   uint32_t minVersion) const;

    mutable std::shared_mutex mtx_;
    std::map<std::string, Entry> entries_;
};

} // namespace zenith::plugin
