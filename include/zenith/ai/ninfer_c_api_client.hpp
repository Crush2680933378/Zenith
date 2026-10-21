// ============================================================================
//  zenith::ai::wrapNinferCAbiService —— 宿主侧适配器（MinGW 编译）
//
//  插件（MSVC 侧）注册的是**纯 C 服务表** zenith_nz_service；宿主拿到后用它构造一个
//  实现了 IInferenceService 的 C++ 对象。C++ 虚接口只存在于宿主侧，从不跨 DLL。
// ============================================================================
#pragma once

#include <memory>
#include <string>

#include "zenith/ai/inference_service.hpp"
#include "zenith/ai/ninfer_c_api.h"
#include "zenith/core/export.hpp"

namespace zenith::ai {

/// serviceTable：注册表里查到的 void*（指向 zenith_nz_service）。
/// 版本不符或表为空 → 返回 nullptr。
ZENITH_API std::unique_ptr<IInferenceService> wrapNinferCAbiService(const void* serviceTable,
                                                                   std::string modelId,
                                                                   std::string artifactPath = {});

} // namespace zenith::ai
