#include "zenith/net/signal.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

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
  #include <csignal>
#endif

namespace zenith::net {

namespace {
std::function<void()> g_handler;
std::once_flag g_once;

#if defined(ZENITH_PLATFORM_WINDOWS)
BOOL WINAPI consoleCtrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_SHUTDOWN_EVENT) {
        if (g_handler) g_handler();
        return TRUE;
    }
    return FALSE;
}
#else
std::atomic<bool> g_fired{false};
void posixSignal(int) {
    g_fired.store(true); // 处理器内只置位；轮询线程触发回调
}
#endif
} // namespace

bool installSignalHandler(std::function<void()> handler) {
    g_handler = std::move(handler);
#if defined(ZENITH_PLATFORM_WINDOWS)
    return SetConsoleCtrlHandler(&consoleCtrl, TRUE) != FALSE;
#else
    std::call_once(g_once, [] {
        struct sigaction sa{};
        sa.sa_handler = &posixSignal;
        sigaction(SIGINT, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        std::thread([] {
            for (;;) {
                if (g_fired.exchange(false)) {
                    if (g_handler) g_handler();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }).detach();
    });
    return true;
#endif
}

} // namespace zenith::net
