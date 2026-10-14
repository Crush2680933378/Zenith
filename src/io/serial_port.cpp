#include "zenith/io/serial_port.hpp"

#include <cstring>

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
  #include <dirent.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <string.h>
  #include <termios.h>
  #include <unistd.h>
#endif

namespace zenith::io {

#if defined(ZENITH_PLATFORM_WINDOWS)

SerialPort::~SerialPort() { close(); }

core::Result<void> SerialPort::open(const std::string& name, const SerialOptions& opt) {
    close();
    // 规范名：COM3 → \\.\COM3（COM10+ 必需；COM1-9 兼容）
    std::string dev = name;
    if (name.compare(0, 4, "\\\\.\\") != 0) dev = "\\\\.\\" + name;

    HANDLE h = CreateFileA(dev.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        return core::Result<void>::Err(
            core::Error(100, "serial open failed (" + std::to_string(GetLastError()) + "): " + name));
    }
    fd_ = reinterpret_cast<uintptr_t>(h);

    DCB dcb{};
    dcb.DCBlength = sizeof(DCB);
    GetCommState(h, &dcb);
    dcb.BaudRate = opt.baud;
    dcb.ByteSize = opt.dataBits;
    dcb.Parity = opt.parity == 'N' ? NOPARITY : (opt.parity == 'E' ? EVENPARITY : ODDPARITY);
    dcb.StopBits = opt.stopBits == 2 ? TWOSTOPBITS : ONESTOPBIT;
    dcb.fBinary = TRUE;
    dcb.fParity = opt.parity != 'N' ? TRUE : FALSE;
    if (!SetCommState(h, &dcb)) {
        close();
        return core::Result<void>::Err(core::Error(101, "serial SetCommState failed"));
    }

    COMMTIMEOUTS t{};
    t.ReadIntervalTimeout = 50;          // 字符间隔
    t.ReadTotalTimeoutConstant = static_cast<DWORD>(opt.readTimeoutMs);
    t.ReadTotalTimeoutMultiplier = 0;
    t.WriteTotalTimeoutConstant = 3000;
    t.WriteTotalTimeoutMultiplier = 0;
    if (!SetCommTimeouts(h, &t)) {
        close();
        return core::Result<void>::Err(core::Error(102, "serial SetCommTimeouts failed"));
    }
    return core::Result<void>::Ok();
}

void SerialPort::close() {
    if (fd_ != kInvalidSerial) {
        CloseHandle(reinterpret_cast<HANDLE>(fd_));
        fd_ = kInvalidSerial;
    }
}

core::Result<size_t> SerialPort::readSome(uint8_t* buf, size_t cap) {
    if (fd_ == kInvalidSerial) {
        return core::Result<size_t>::Err(core::Error(103, "serial not open"));
    }
    DWORD got = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(fd_), buf, static_cast<DWORD>(cap), &got, nullptr)) {
        return core::Result<size_t>::Err(
            core::Error(104, "serial read failed (" + std::to_string(GetLastError()) + ")"));
    }
    return core::Result<size_t>::Ok(static_cast<size_t>(got));
}

core::Result<size_t> SerialPort::writeAll(const uint8_t* buf, size_t n) {
    if (fd_ == kInvalidSerial) {
        return core::Result<size_t>::Err(core::Error(103, "serial not open"));
    }
    size_t done = 0;
    while (done < n) {
        DWORD wrote = 0;
        if (!WriteFile(reinterpret_cast<HANDLE>(fd_), buf + done,
                       static_cast<DWORD>(n - done), &wrote, nullptr)) {
            return core::Result<size_t>::Err(
                core::Error(105, "serial write failed (" + std::to_string(GetLastError()) + ")"));
        }
        if (wrote == 0) break;
        done += wrote;
    }
    return core::Result<size_t>::Ok(done);
}

std::vector<std::string> SerialPort::enumerate() {
    std::vector<std::string> out;
    // QueryDosDeviceA 枚举 NT 设备名，筛选 COM*
    char buf[64 * 1024];
    const DWORD len = QueryDosDeviceA(nullptr, buf, sizeof(buf));
    if (len == 0) return out;
    size_t pos = 0;
    while (pos < len) {
        const std::string entry(buf + pos);
        pos += entry.size() + 1;
        if (entry.empty()) break;
        if (entry.compare(0, 3, "COM") == 0) {
            // 排除 COM 口映射的内核服务（如 LPT → 并口）
            bool digit = true;
            for (char c : entry.substr(3)) {
                if (c < '0' || c > '9') { digit = false; break; }
            }
            if (digit) out.push_back(entry);
        }
    }
    return out;
}

#else // POSIX

SerialPort::~SerialPort() { close(); }

core::Result<void> SerialPort::open(const std::string& name, const SerialOptions& opt) {
    close();
    const int fd = ::open(name.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return core::Result<void>::Err(
            core::Error(100, std::string("serial open failed: ") + strerror(errno)));
    }
    fd_ = static_cast<uintptr_t>(fd);

    termios tio{};
    if (tcgetattr(fd, &tio) != 0) {
        close();
        return core::Result<void>::Err(core::Error(101, "serial tcgetattr failed"));
    }
    speed_t speed = B115200;
    switch (opt.baud) {
        case 9600: speed = B9600; break;
        case 19200: speed = B19200; break;
        case 38400: speed = B38400; break;
        case 57600: speed = B57600; break;
        case 115200: speed = B115200; break;
        case 230400: speed = B230400; break;
        default: speed = B115200; break;
    }
    cfsetispeed(&tio, speed);
    cfsetospeed(&tio, speed);
    tio.c_cflag = (tio.c_cflag & ~CSIZE) | (opt.dataBits == 7 ? CS7 : CS8);
    if (opt.stopBits == 2) tio.c_cflag |= CSTOPB;
    if (opt.parity != 'N') {
        tio.c_cflag |= PARENB;
        if (opt.parity == 'O') tio.c_cflag |= PARODD;
    }
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG); // 原始模式
    tio.c_iflag &= ~(IXON | IXOFF | IXANY);
    tio.c_oflag &= ~OPOST;
    tio.c_cc[VTIME] = static_cast<cc_t>(opt.readTimeoutMs / 100);
    tio.c_cc[VMIN] = 0;
    if (tcsetattr(fd, TCSANOW, &tio) != 0) {
        close();
        return core::Result<void>::Err(core::Error(102, "serial tcsetattr failed"));
    }
    return core::Result<void>::Ok();
}

void SerialPort::close() {
    if (fd_ != kInvalidSerial) {
        ::close(static_cast<int>(fd_));
        fd_ = kInvalidSerial;
    }
}

core::Result<size_t> SerialPort::readSome(uint8_t* buf, size_t cap) {
    if (fd_ == kInvalidSerial) {
        return core::Result<size_t>::Err(core::Error(103, "serial not open"));
    }
    const ssize_t n = ::read(static_cast<int>(fd_), buf, cap);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return core::Result<size_t>::Ok(size_t{0});
        }
        return core::Result<size_t>::Err(
            core::Error(104, std::string("serial read failed: ") + strerror(errno)));
    }
    return core::Result<size_t>::Ok(static_cast<size_t>(n));
}

core::Result<size_t> SerialPort::writeAll(const uint8_t* buf, size_t n) {
    if (fd_ == kInvalidSerial) {
        return core::Result<size_t>::Err(core::Error(103, "serial not open"));
    }
    size_t done = 0;
    while (done < n) {
        const ssize_t w = ::write(static_cast<int>(fd_), buf + done, n - done);
        if (w < 0) {
            if (errno == EAGAIN) continue;
            return core::Result<size_t>::Err(
                core::Error(105, std::string("serial write failed: ") + strerror(errno)));
        }
        done += static_cast<size_t>(w);
    }
    return core::Result<size_t>::Ok(done);
}

std::vector<std::string> SerialPort::enumerate() {
    std::vector<std::string> out;
    const char* dirs[] = {"/dev"};
    const char* prefixes[] = {"ttyS", "ttyUSB", "ttyACM", "ttyAMA"};
    for (const char* d : dirs) {
        DIR* dp = opendir(d);
        if (dp == nullptr) continue;
        while (dirent* e = readdir(dp)) {
            const std::string name = e->d_name;
            for (const char* p : prefixes) {
                if (name.compare(0, strlen(p), p) == 0) {
                    out.push_back(std::string(d) + "/" + name);
                    break;
                }
            }
        }
        closedir(dp);
    }
    return out;
}

#endif

} // namespace zenith::io
