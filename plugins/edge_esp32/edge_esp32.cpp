// ============================================================================
//  edge_esp32 —— C ABI 插件：边缘设备桥（TCP 网关 + COBS 帧 + 心跳/RPC）
//  自包含实现：独立线程 + 原生 socket（不依赖宿主 EventLoop——纯 C 边界内自治），
//  串口通道走 zenith::io::SerialPort（仅头+实现拷贝内联编译，同工具链）。
//  测试：tests/test_edge.cpp 用 loopback TCP 模拟 ESP32 设备全链路验证。
// ============================================================================
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "zenith/abi/zenith_abi.h"

// 同工具链内联编译（插件不链接 zenith 动态库——保持纯 C 边界自治）
#include "zenith/core/string.hpp"
#include "zenith/io/cobs.hpp"
#include "zenith/io/serial_port.hpp"

#include "device_service.hpp"

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using SocketFd = SOCKET;
  #define EDGE_INVALID INVALID_SOCKET
  #define EDGE_WOULD(e) ((e) == WSAEWOULDBLOCK)
#else
  #include <arpa/inet.h>
  #include <errno.h>
  #include <fcntl.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketFd = int;
  #define EDGE_INVALID (-1)
  #define EDGE_WOULD(e) ((e) == EAGAIN || (e) == EWOULDBLOCK)
#endif

namespace {

const zenith_host_api* g_host = nullptr;

void logInfo(const std::string& m) {
    if (g_host) g_host->log_write(g_host->log_ctx, ZENITH_LOG_INFO, "edge_esp32", 0, m.c_str());
}
void logWarn(const std::string& m) {
    if (g_host) g_host->log_write(g_host->log_ctx, ZENITH_LOG_WARN, "edge_esp32", 0, m.c_str());
}

void setNonBlocking(SocketFd s) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    const int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif
}

int sockErrno() {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

#if defined(_WIN32)
struct WsaInit {
    WsaInit() {
        WSADATA d;
        WSAStartup(MAKEWORD(2, 2), &d);
    }
};
#endif

/// 极简 JSON 字符串字段抽取（设备侧载荷仅扁平对象；避免插件依赖完整 JSON）
std::string jsonStrField(const std::string& j, const std::string& key) {
    const std::string pat = "\"" + key + "\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p = j.find(':', p + pat.size());
    if (p == std::string::npos) return "";
    ++p;
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    if (p >= j.size()) return "";
    if (j[p] == '"') {
        const size_t e = j.find('"', p + 1);
        if (e == std::string::npos) return "";
        return j.substr(p + 1, e - p - 1);
    }
    const size_t e = j.find_first_of(",}", p);
    return j.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// ---------------------------------------------------------------------------
class DeviceBridgeImpl final : public zenith::edge::IDeviceBridge {
public:
    DeviceBridgeImpl() = default;
    ~DeviceBridgeImpl() override { stop(); }

    int startTcpGateway(const std::string& host, uint16_t port) override {
        std::lock_guard<std::mutex> lk(mtx_);
        if (listenFd_ != EDGE_INVALID) return -1;
#if defined(_WIN32)
        static WsaInit wsaInit;
#endif
        listenFd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenFd_ == EDGE_INVALID) return -1;
        int one = 1;
        setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&one), sizeof(one));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr =
            (host.empty() || host == "0.0.0.0")
                ? htonl(INADDR_ANY)
                : inet_addr(host.c_str());
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
            ::listen(listenFd_, 16) != 0) {
            logWarn("gateway bind/listen failed");
            closesock(listenFd_);
            listenFd_ = EDGE_INVALID;
            return -1;
        }
        // 实际端口
        socklen_t len = sizeof(addr);
        uint16_t actual = port;
        if (getsockname(listenFd_, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            actual = ntohs(addr.sin_port);
        }
        setNonBlocking(listenFd_);
        running_.store(true);
        thread_ = std::thread([this] { runLoop(); });
        gatewayPort_ = actual;
        logInfo("edge gateway listening on port " + std::to_string(actual));
        return static_cast<int>(actual);
    }

    bool attachSerial(const std::string& comName, uint32_t baud) override {
        auto* sp = new zenith::io::SerialPort();
        zenith::io::SerialOptions opt;
        opt.baud = baud;
        auto r = sp->open(comName, opt);
        if (!r) {
            delete sp;
            logWarn("serial attach failed: " + comName);
            return false;
        }
        std::lock_guard<std::mutex> lk(mtx_);
        serials_.emplace_back(sp);
        return true;
    }

    std::vector<zenith::edge::DeviceInfo> devices() override {
        std::lock_guard<std::mutex> lk(mtx_);
        std::vector<zenith::edge::DeviceInfo> out;
        const int64_t now = nowMs();
        for (const auto& [id, d] : devices_) {
            zenith::edge::DeviceInfo info;
            info.id = id;
            info.transport = d.tcp ? "tcp" : "serial";
            info.address = d.address;
            info.lastSeenMs = d.lastSeen;
            info.online = now - d.lastSeen < kOfflineMs;
            out.push_back(info);
        }
        return out;
    }

    bool sendRpc(const std::string& deviceId, const std::string& method,
                 const std::string& argsJson, RpcCallback cb, int timeoutMs) override {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = devices_.find(deviceId);
        if (it == devices_.end() || it->second.fd == EDGE_INVALID) return false;
        const uint32_t rid = nextRid_++;
        pending_[rid] = PendingRpc{cb, nowMs() + timeoutMs};
        it->second.writeBuf += zenith::io::frame::encode(
            "{\"t\":\"rpc\",\"rid\":" + std::to_string(rid) + ",\"method\":\"" + method +
            "\",\"args\":" + (argsJson.empty() ? "{}" : argsJson) + "}");
        return true;
    }

    void setEventHandler(EventHandler h) override {
        std::lock_guard<std::mutex> lk(mtx_);
        eventHandler_ = std::move(h);
    }

    void stop() override {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!running_.exchange(false)) return;
        }
        if (thread_.joinable()) thread_.join();
        std::lock_guard<std::mutex> lk(mtx_);
        if (listenFd_ != EDGE_INVALID) {
            closesock(listenFd_);
            listenFd_ = EDGE_INVALID;
        }
        for (auto& [id, d] : devices_) {
            if (d.fd != EDGE_INVALID) closesock(d.fd);
        }
        devices_.clear();
        for (auto* sp : serials_) delete sp;
        serials_.clear();
    }

private:
    static constexpr int64_t kOfflineMs = 10000;
    static constexpr int kTickMs = 50;

    struct Device {
        bool tcp = true;
        SocketFd fd = EDGE_INVALID;
        std::string address;
        int64_t lastSeen = 0;
        std::string pendingName; // hello 待注册名（由持有迭代的循环执行转移）
        zenith::io::frame::Deframer deframer;
        std::string writeBuf;
    };
    struct PendingRpc {
        RpcCallback cb;
        int64_t deadline = 0;
    };

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    static void closesock(SocketFd s) {
#if defined(_WIN32)
        ::closesocket(s);
#else
        ::close(s);
#endif
    }

    void sendRaw(const std::string& id, const std::string& wire) {
        // 调用方持锁
        auto it = devices_.find(id);
        if (it == devices_.end()) return;
        it->second.writeBuf += wire;
    }

    void runLoop() {
        std::vector<SocketFd> fds;
        while (running_.load()) {
            // 1) accept
            if (listenFd_ != EDGE_INVALID) {
                for (;;) {
                    sockaddr_in peer{};
                    socklen_t plen = sizeof(peer);
                    const SocketFd c = ::accept(listenFd_,
                                                reinterpret_cast<sockaddr*>(&peer), &plen);
                    if (c == EDGE_INVALID) break;
                    setNonBlocking(c);
                    int one = 1;
                    setsockopt(c, IPPROTO_TCP, TCP_NODELAY,
                               reinterpret_cast<const char*>(&one), sizeof(one));
                    char ip[64] = {0};
                    inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
                    std::lock_guard<std::mutex> lk(mtx_);
                    auto d = std::make_unique<Device>();
                    d->tcp = true;
                    d->fd = c;
                    d->address = std::string(ip) + ":" + std::to_string(ntohs(peer.sin_port));
                    d->lastSeen = nowMs();
                    anon_.push_back(std::move(d));
                }
            }
            // 2) 串口读
            {
                std::lock_guard<std::mutex> lk(mtx_);
                for (auto* sp : serials_) {
                    uint8_t buf[4096];
                    auto r = sp->readSome(buf, sizeof(buf));
                    if (r && r.value() > 0) {
                        std::string id = "serial:" + std::to_string(reinterpret_cast<uintptr_t>(sp));
                        Device& d = devices_[id];
                        d.tcp = false;
                        d.address = "serial";
                        d.lastSeen = nowMs();
                        handleFrames(d, id, buf, r.value());
                    }
                }
            }
            // 3) 设备 socket 读/写 + 超时
            {
                std::vector<std::string> events;
                std::map<uint32_t, RpcCallback> timeouts;
                std::vector<std::string> gone;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    const int64_t now = nowMs();

                    // RPC 超时
                    for (auto it = pending_.begin(); it != pending_.end();) {
                        if (now > it->second.deadline) {
                            timeouts[it->first] = std::move(it->second.cb);
                            it = pending_.erase(it);
                        } else {
                            ++it;
                        }
                    }

                    auto promote = [&](size_t i) {
                        Device& d = *anon_[i];
                        if (d.pendingName.empty()) return;
                        const std::string name = std::move(d.pendingName);
                        d.pendingName.clear();
                        Device named = std::move(d);
                        devices_[name] = std::move(named);
                        anon_.erase(anon_.begin() + static_cast<long>(i));
                    };
                    auto sweep = [&](const std::string& id, Device& d) {
                        if (d.fd == EDGE_INVALID) return;
                        uint8_t buf[16384];
                        for (;;) {
                            const int n = ::recv(d.fd, reinterpret_cast<char*>(buf), sizeof(buf), 0);
                            if (n > 0) {
                                d.lastSeen = now;
                                handleFrames(d, id, buf, static_cast<size_t>(n));
                                continue;
                            }
                            if (n == 0 || !EDGE_WOULD(sockErrno())) {
                                closesock(d.fd);
                                d.fd = EDGE_INVALID;
                                gone.push_back(id);
                            }
                            break;
                        }
                        // 写冲刷
                        if (!d.writeBuf.empty() && d.fd != EDGE_INVALID) {
                            const int w = ::send(d.fd, d.writeBuf.data(),
                                                 static_cast<int>(d.writeBuf.size()), 0);
                            if (w > 0) d.writeBuf.erase(0, static_cast<size_t>(w));
                        }
                    };
                    for (auto& [id, d] : devices_) sweep(id, d);
                    for (size_t i = 0; i < anon_.size();) {
                        sweep("", *anon_[i]);
                        if (!anon_[i]->pendingName.empty()) {
                            promote(i); // hello 注册：转移后不递增（元素已删除）
                            continue;
                        }
                        if (anon_[i]->fd == EDGE_INVALID) {
                            anon_.erase(anon_.begin() + static_cast<long>(i));
                        } else {
                            ++i;
                        }
                    }
                }
                for (auto& [rid, cb] : timeouts) {
                    if (cb) cb(false, "{\"error\":\"timeout\"}");
                }
                for (const auto& id : gone) {
                    logInfo("device offline: " + id);
                    // 设备掉线保留条目（online=false），由查询方呈现
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
        }
    }

    /// 拆帧 + 派发（调用方持锁）
    void handleFrames(Device& d, const std::string& knownId, const uint8_t* data, size_t n) {
        for (const std::string& payload : d.deframer.feed(data, n)) {
            const std::string t = jsonStrField(payload, "t");
            if (t == "hello") {
                const std::string id = jsonStrField(payload, "id");
                if (id.empty()) continue;
                d.lastSeen = nowMs();
                if (knownId.empty()) {
                    d.pendingName = id; // 由 sweep 循环（迭代持有者）完成转移
                }
                logInfo("device online: " + id);
                fireEvent(id, payload);
            } else if (t == "hb") {
                const std::string id = jsonStrField(payload, "id");
                auto it = devices_.find(id);
                if (it != devices_.end()) it->second.lastSeen = nowMs();
                fireEvent(id, payload);
            } else if (t == "resp") {
                const uint32_t rid = static_cast<uint32_t>(
                    std::strtoul(jsonStrField(payload, "rid").c_str(), nullptr, 10));
                auto it = pending_.find(rid);
                if (it != pending_.end()) {
                    RpcCallback cb = std::move(it->second.cb);
                    pending_.erase(it);
                    if (cb) cb(jsonStrField(payload, "ok") != "false", payload);
                }
            } else if (t == "event") {
                fireEvent(jsonStrField(payload, "id"), payload);
            }
        }
    }

    void fireEvent(const std::string& id, const std::string& json) {
        if (eventHandler_) eventHandler_(id, json);
    }

    std::mutex mtx_;
    SocketFd listenFd_ = EDGE_INVALID;
    uint16_t gatewayPort_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::map<std::string, Device> devices_;
    std::vector<std::unique_ptr<Device>> anon_; // 未 hello 的接入连接
    std::vector<zenith::io::SerialPort*> serials_;
    std::map<uint32_t, PendingRpc> pending_;
    uint32_t nextRid_ = 1;
    EventHandler eventHandler_;
};

void destroyBridge(void* p) { delete static_cast<DeviceBridgeImpl*>(p); }

zenith_status pluginInit(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, "edge_esp32", 0,
                    "edge_esp32: init (COBS+CRC32 framing, tcp gateway + serial)");
    auto* svc = new (std::nothrow) DeviceBridgeImpl();
    if (svc == nullptr) return ZENITH_ERR_OUT_OF_MEMORY;
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  zenith::edge::IDeviceBridge::service_name(),
                                  zenith::edge::IDeviceBridge::service_version(),
                                  static_cast<void*>(svc), &destroyBridge);
}

zenith_status pluginStart(void) { return ZENITH_OK; }
zenith_status pluginStop(void) { return ZENITH_OK; }
void pluginShutdown(void) { g_host = nullptr; }

zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc),
    ZENITH_ABI_VERSION,
    "edge_esp32",
    "0.1.0",
    "Zenith edge plugin: COBS-framed device bridge (TCP gateway + serial)",
    "Zenith Authors",
    &pluginInit,
    &pluginStart,
    &pluginStop,
    &pluginShutdown,
    {nullptr, nullptr, nullptr, nullptr},
};

} // namespace

ZENITH_PLUGIN_EXPORT(g_desc)
