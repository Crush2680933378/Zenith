// ============================================================================
//  DynLib —— 动态库加载的跨平台 RAII 封装
//  Windows: LoadLibraryA / GetProcAddress / FreeLibrary
//  POSIX   : dlopen(RTLD_NOW|RTLD_LOCAL) / dlsym / dlclose
// ============================================================================
#pragma once

#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"

namespace zenith::plugin {

class ZENITH_API DynLib : private core::NonCopyable {
public:
    explicit DynLib(std::string path);

    /// 幂等：已加载时直接成功。失败保留错误信息。
    core::Result<void> load();

    /// 查找符号；未加载或不存在返回 nullptr（并记录 lastError）。
    void* symbol(const char* name);

    void unload();

    [[nodiscard]] bool loaded() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] const std::string& lastError() const noexcept { return lastError_; }

private:
    void storePlatformError(const char* context);

    std::string path_;
    void*       handle_    = nullptr;
    std::string lastError_;
};

/// 插件动态库在当前平台的扩展名（含点），如 ".dll" / ".so" / ".dylib"。
ZENITH_API const char* pluginFileExtension() noexcept;

} // namespace zenith::plugin
