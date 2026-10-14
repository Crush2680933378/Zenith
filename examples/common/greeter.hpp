// ============================================================================
//  示例共享接口：宿主与示例插件共同包含
//  - IGreeter   ：C++ 虚接口形态（同一工具链编译的插件使用）
//  - CGreeter   ：纯 C 函数表形态（任何 C ABI 兼容编译器均可实现/消费）
//  两种形态演示 Zenith 服务边界的两条路径。
// ============================================================================
#pragma once

#include <stddef.h>
#include <stdint.h>

/* ---- 纯 C 部分：C 与 C++ 均可见 ---- */
#ifdef __cplusplus
extern "C" {
#endif

#define ZENITH_C_GREETER_NAME "zenith.example.CGreeter"
#define ZENITH_C_GREETER_VERSION 1u

typedef struct zenith_c_greeter_vt {
    /// 把问候语写入 buf（不超过 cap），返回 buf；失败返回 NULL。
    const char* (*greet)(void* self, const char* name, char* buf, size_t cap);
} zenith_c_greeter_vt;

typedef struct zenith_c_greeter {
    const zenith_c_greeter_vt* vt;
} zenith_c_greeter;

#ifdef __cplusplus
} /* extern "C" */
#endif

/* ---- C++ 部分 ---- */
#ifdef __cplusplus
#include <string>
#include "zenith/plugin/service.hpp"

namespace zenith::example {

class IGreeter : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(IGreeter, "zenith.example.IGreeter", 1)
    [[nodiscard]] virtual std::string greet(const std::string& name) const = 0;
};

} // namespace zenith::example
#endif
