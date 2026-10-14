// ============================================================================
//  IService —— Zenith 服务基类（仅限同一工具链编译单元之间使用）
//
//  重要边界规则：
//    - C++ 虚接口（本头文件）只在「宿主与插件用同一编译器/运行库构建」时
//      允许穿越动态库边界；跨编译器场景必须降级到纯 C ABI（abi/zenith_abi.h
//      中的 register_service/query_service + C 函数表）。
//    - 注册键 = service_name() 字符串 + service_version() 版本号，
//      查询方以「所需最低版本」协商。
// ============================================================================
#pragma once

#include <cstdint>

namespace zenith::plugin {

class IService {
public:
    virtual ~IService() = default;

    /// 接口名（稳定字符串，如 "zenith.net.IEventLoop"）。
    [[nodiscard]] virtual const char* serviceInterfaceName() const noexcept = 0;
    /// 实现方面向的接口版本。
    [[nodiscard]] virtual uint32_t serviceInterfaceVersion() const noexcept = 0;
};

} // namespace zenith::plugin

/// 声明服务的静态接口信息（配合 ServiceRegistry 的模板 API）。
/// 用法：
///   class IGreeter : public zenith::plugin::IService {
///    public:
///     ZENITH_SERVICE_DECL(IGreeter, "zenith.example.IGreeter", 1)
///     virtual std::string greet(const std::string& name) const = 0;
///   };
#define ZENITH_SERVICE_DECL(Iface, if_name, if_version)                             \
    static constexpr const char* service_name() noexcept { return if_name; }         \
    static constexpr uint32_t service_version() noexcept { return if_version; }      \
    [[nodiscard]] const char* serviceInterfaceName() const noexcept override         \
    {                                                                                \
        return service_name();                                                       \
    }                                                                                \
    [[nodiscard]] uint32_t serviceInterfaceVersion() const noexcept override         \
    {                                                                                \
        return service_version();                                                    \
    }
