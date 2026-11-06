#include "zenith/plugin/dynlib.hpp"

#include <utility>

#include "zenith/core/platform.hpp"

#if defined(ZENITH_PLATFORM_WINDOWS)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <windows.h>
#else
  #include <dlfcn.h>
#endif

namespace zenith::plugin {

const char* pluginFileExtension() noexcept {
#if defined(ZENITH_PLATFORM_WINDOWS)
    return ".dll";
#elif defined(ZENITH_PLATFORM_MACOS)
    // macOS 上 add_library(... MODULE) 产出的是 .so（CMAKE_SHARED_MODULE_SUFFIX），
    // 只有 SHARED 才是 .dylib。插件的构建形态是 MODULE，所以这里必须是 .so——
    // 返回 .dylib 会「插件构建成功但加载器一个都扫不到」。
    return ".so";
#else
    return ".so";
#endif
}

DynLib::DynLib(std::string path) : path_(std::move(path)) {}

core::Result<void> DynLib::load() {
    if (handle_ != nullptr) return core::Result<void>::Ok();
    if (path_.empty()) {
        lastError_ = "DynLib: empty path";
        return core::Result<void>::Err(core::Error(1, lastError_));
    }

#if defined(ZENITH_PLATFORM_WINDOWS)
    // LOAD_WITH_ALTERED_SEARCH_PATH：让 Windows 解析**该 DLL 自身的依赖**时，
    // 先去它所在目录找。默认搜索顺序只看 exe 目录/系统目录/PATH——插件把私有
    // 依赖 DLL 放在自己旁边（例如 CUDA/FFmpeg 运行时）时会一律 126 加载失败。
    handle_ = static_cast<void*>(LoadLibraryExA(path_.c_str(), nullptr,
                                                LOAD_WITH_ALTERED_SEARCH_PATH));
#else
    handle_ = dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (handle_ == nullptr) {
        storePlatformError("load");
        return core::Result<void>::Err(core::Error(2, lastError_));
    }
    return core::Result<void>::Ok();
}

void* DynLib::symbol(const char* name) {
    if (handle_ == nullptr || name == nullptr || name[0] == '\0') {
        lastError_ = "DynLib: not loaded or empty symbol name";
        return nullptr;
    }
#if defined(ZENITH_PLATFORM_WINDOWS)
    void* sym = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle_), name));
#else
    void* sym = dlsym(handle_, name);
#endif
    if (sym == nullptr) {
        lastError_ = std::string("DynLib: symbol not found: ") + name;
    }
    return sym;
}

void DynLib::unload() {
    if (handle_ == nullptr) return;
#if defined(ZENITH_PLATFORM_WINDOWS)
    FreeLibrary(static_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void DynLib::storePlatformError(const char* context) {
#if defined(ZENITH_PLATFORM_WINDOWS)
    const DWORD code = GetLastError();
    LPSTR buf   = nullptr;
    const DWORD len = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                         FORMAT_MESSAGE_IGNORE_INSERTS,
                                     nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<LPSTR>(&buf), 0, nullptr);
    lastError_ = std::string("DynLib ") + context + " failed (code " + std::to_string(code) + "): " +
                 (buf != nullptr && len > 0 ? std::string(buf, len) : "unknown error");
    if (buf != nullptr) LocalFree(buf);
#else
    const char* err = dlerror();
    lastError_       = std::string("DynLib ") + context + " failed: " + (err ? err : "unknown error");
#endif
}

} // namespace zenith::plugin
