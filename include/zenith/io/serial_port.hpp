// ============================================================================
//  zenith::io::SerialPort —— 串口（win32 CreateFile / POSIX termios 双后端）
//  - 阻塞式超时读写在专用线程上使用（edge 插件模式）；
//    Windows 串口句柄不能进 WSAPoll，故不做 EventLoop 集成（文档已注明）
//  - 端口枚举：Windows 走 QueryDosDevice；POSIX 扫 /dev/ttyS*、/dev/ttyUSB*、/dev/ttyACM*
// ============================================================================
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"
#include "zenith/core/result.hpp"

namespace zenith::io {

struct SerialOptions {
    uint32_t baud = 115200;
    uint8_t dataBits = 8;    // 7/8
    char parity = 'N';       // N/E/O
    uint8_t stopBits = 1;    // 1/2
    int readTimeoutMs = 200; // 单次读超时（0 = 立即返回）
};

class ZENITH_API SerialPort : private core::NonCopyable {
public:
    SerialPort() = default;
    ~SerialPort();

    /// 打开串口（Windows 名如 "COM3"；POSIX 如 "/dev/ttyUSB0"）。
    core::Result<void> open(const std::string& name, const SerialOptions& opt = {});
    void close();
    [[nodiscard]] bool isOpen() const noexcept { return fd_ != kInvalidSerial; }

    /// 读到至多 cap 字节；超时返回 0；错误 Err。
    core::Result<size_t> readSome(uint8_t* buf, size_t cap);
    /// 写全部字节；返回写出数或 Err。
    core::Result<size_t> writeAll(const uint8_t* buf, size_t n);
    core::Result<size_t> writeAll(const std::string& s) {
        return writeAll(reinterpret_cast<const uint8_t*>(s.data()), s.size());
    }

    /// 枚举可用串口名。
    [[nodiscard]] static std::vector<std::string> enumerate();

private:
    static constexpr uintptr_t kInvalidSerial = static_cast<uintptr_t>(-1);
    uintptr_t fd_ = kInvalidSerial;
};

} // namespace zenith::io
