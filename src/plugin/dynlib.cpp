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
    return ".dylib";
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
    handle_ = static_cast<void*>(LoadLibraryA(path_.c_str()));
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
