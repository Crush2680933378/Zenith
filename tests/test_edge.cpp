// edge 插件测试：COBS/CRC 单测 + 模拟 ESP32 设备（loopback TCP）全链路
#include <atomic>
#include <chrono>
#include <thread>

#if defined(_WIN32)
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
  #endif
  #include <winsock2.h>
  #include <ws2tcpip.h>
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
#endif

#include "ztest.hpp"

#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/io/cobs.hpp"
#include "zenith/io/serial_port.hpp"
#include "zenith/plugin/plugin_manager.hpp"
#include "zenith/plugin/service_registry.hpp"

#include "device_service.hpp"

using namespace zenith;
using namespace std::chrono_literals;

/* ---------------- COBS / CRC ---------------- */

ZTEST(Cobs, EncodeDecodeRoundtrip) {
    // 已知向量（COBS 论文例）：{0x00} → {0x01 0x01}
    ZEXPECT_EQ(io::cobsEncode(std::string("\x00", 1)), std::string("\x01\x01", 2));
    // {0x00 0x00} → {0x01 0x01 0x01}
    ZEXPECT_EQ(io::cobsEncode(std::string("\x00\x00", 2)), std::string("\x01\x01\x01", 3));
    // {0x11 0x22 0x00 0x33} → {0x03 0x11 0x22 0x02 0x33}
    ZEXPECT_EQ(io::cobsEncode(std::string("\x11\x22\x00\x33", 4)),
               std::string("\x03\x11\x22\x02\x33", 5));
    // 无零数据 → 前缀 0xFF 风格
    const std::string data = "\x01\x02\x03\x04";
    auto back = io::cobsDecode(io::cobsEncode(data));
    ZEXPECT_TRUE(back.has_value());
    ZEXPECT_EQ(back.value(), data);

    // 随机含零字节回环
    std::string rnd(1000, '\0');
    for (size_t i = 0; i < rnd.size(); ++i) {
        rnd[i] = static_cast<char>((i * 37 + 11) % 256); // 周期性含 0x00
    }
    auto rb = io::cobsDecode(io::cobsEncode(rnd));
    ZEXPECT_TRUE(rb.has_value());
    ZEXPECT_EQ(rb.value(), rnd);
}

ZTEST(Cobs, LongRunOfZeros) {
    const std::string zeros(600, '\0');
    auto enc = io::cobsEncode(zeros);
    auto dec = io::cobsDecode(enc);
    ZEXPECT_TRUE(dec.has_value());
    ZEXPECT_EQ(dec.value().size(), size_t{600});
}

ZTEST(Crc32, KnownVectors) {
    // CRC32("") = 0
    ZEXPECT_EQ(io::crc32("", 0), uint32_t{0x00000000u});
    // CRC32("a") = 0xE8B7BE43（IEEE 反射）
    ZEXPECT_EQ(io::crc32("a", 0), uint32_t{0xE8B7BE43u});
    // CRC32("abc") = 0x352441C2
    ZEXPECT_EQ(io::crc32("abc", 0), uint32_t{0x352441C2u});
    // CRC32("123456789") = 0xCBF43926（标准检验向量）
    ZEXPECT_EQ(io::crc32("123456789", 0), uint32_t{0xCBF43926u});
}

ZTEST(Frame, EncodeDecodeWithCrc) {
    const std::string payload = R"({"t":"hb","id":"esp-1"})";
    const std::string wire = io::frame::encode(payload);
    ZEXPECT_TRUE(wire.back() == '\0');
    auto back = io::frame::decode(
        reinterpret_cast<const uint8_t*>(wire.data()), wire.size() - 1);
    ZEXPECT_TRUE(back.has_value());
    ZEXPECT_EQ(back.value(), payload);

    // 破坏一字节 → CRC 拒绝
    std::string corrupt = wire;
    corrupt[corrupt.size() / 2] = static_cast<char>(corrupt[corrupt.size() / 2] ^ 0x55);
    auto bad = io::frame::decode(
        reinterpret_cast<const uint8_t*>(corrupt.data()), corrupt.size() - 1);
    ZEXPECT_TRUE(!bad.has_value());
}

ZTEST(Deframer, StreamSplitAndMultiple) {
    io::frame::Deframer df;
    const std::string a = R"({"t":"hb"})";
    const std::string b = R"({"t":"event","name":"x"})";
    const std::string stream = io::frame::encode(a) + io::frame::encode(b);

    // 逐字节喂入
    std::vector<std::string> got;
    for (char c : stream) {
        auto frames = df.feed(&reinterpret_cast<const uint8_t&>(c), 1);
        for (auto& f : frames) got.push_back(f);
    }
    ZEXPECT_EQ(got.size(), size_t{2});
    if (got.size() == 2) {
        ZEXPECT_EQ(got[0], a);
        ZEXPECT_EQ(got[1], b);
    }

    // 一次全喂
    io::frame::Deframer df2;
    auto all = df2.feed(stream);
    ZEXPECT_EQ(all.size(), size_t{2});
}

ZTEST(Serial, EnumerateAndOpenMissing) {
    const auto ports = io::SerialPort::enumerate();
    (void)ports; // 环境相关——仅验证不崩溃
    io::SerialPort sp;
    auto r = sp.open("COM_IMPOSSIBLE_99", io::SerialOptions{});
    ZEXPECT_TRUE(r.isErr());
    ZEXPECT_FALSE(sp.isOpen());
}

/* ---------------- 设备桥端到端（模拟 ESP32）---------------- */

namespace {
/// 模拟设备：阻塞 socket 客户端，COBS 帧 + JSON 载荷
class FakeDevice {
public:
    bool connect(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        return ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
    }
    bool sendJson(const std::string& json) {
        const std::string wire = io::frame::encode(json);
        return ::send(fd_, wire.data(), static_cast<int>(wire.size()), 0) > 0;
    }
    /// 读一帧（JSON 载荷）；超时返回空
    std::string recvJson(int timeoutMs = 3000) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            for (const std::string& f : deframer_.feed(buf_)) return f;
            uint8_t tmp[4096];
            const int n = ::recv(fd_, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
            if (n > 0) {
                buf_.assign(reinterpret_cast<char*>(tmp), static_cast<size_t>(n));
                for (const std::string& f : deframer_.feed(buf_)) return f;
                buf_.clear();
            } else if (n == 0) {
                return "";
            } else {
                std::this_thread::sleep_for(10ms);
            }
        }
        return "";
    }
    ~FakeDevice() { closesock(static_cast<int>(fd_)); }

private:
    static void closesock(int s) {
#if defined(_WIN32)
        ::closesocket(s);
#else
        ::close(s);
#endif
    }
    uintptr_t fd_ = static_cast<uintptr_t>(-1);
    io::frame::Deframer deframer_;
    std::string buf_;
};
} // namespace

ZTEST(EdgePlugin, GatewayHelloHeartbeatRpc) {
#if defined(_WIN32)
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
    concurrency::ThreadPool pool(1);
    plugin::ServiceRegistry registry;
    plugin::PluginManager mgr(registry, &pool, nullptr);
    (void)mgr.loadDirectory(ZENITH_TEST_PLUGIN_DIR);

    void* raw = registry.queryRaw(edge::IDeviceBridge::service_name(),
                                   edge::IDeviceBridge::service_version());
    ZEXPECT_TRUE(raw != nullptr);
    if (raw == nullptr) return;
    auto* bridge = static_cast<edge::IDeviceBridge*>(raw);

    const int port = bridge->startTcpGateway("127.0.0.1", 0);
    ZEXPECT_TRUE(port > 0);
    if (port <= 0) return;

    // 事件收集
    std::mutex evMtx;
    std::vector<std::pair<std::string, std::string>> events;
    bridge->setEventHandler([&](const std::string& id, const std::string& json) {
        std::lock_guard<std::mutex> lk(evMtx);
        events.emplace_back(id, json);
    });

    // 模拟设备接入
    FakeDevice dev;
    ZEXPECT_TRUE(dev.connect(static_cast<uint16_t>(port)));
    ZEXPECT_TRUE(dev.sendJson(R"({"t":"hello","id":"esp32-test-1"})"));
    ZEXPECT_TRUE(dev.sendJson(R"({"t":"hb","id":"esp32-test-1","ms":100})"));

    // 等设备注册可见
    bool seen = false;
    for (int i = 0; i < 100 && !seen; ++i) {
        std::this_thread::sleep_for(20ms);
        for (const auto& d : bridge->devices()) {
            if (d.id == "esp32-test-1" && d.online) seen = true;
        }
    }
    ZEXPECT_TRUE(seen);

    // RPC 往返
    std::atomic<bool> rpcDone{false};
    std::string rpcResp;
    ZEXPECT_TRUE(bridge->sendRpc("esp32-test-1", "led",
                                 R"({"on":true})",
                                 [&](bool ok, const std::string& resp) {
                                     rpcDone.store(true);
                                     rpcResp = resp;
                                 }));

    // 设备收到 rpc 帧 → 回 resp
    const std::string req = dev.recvJson();
    ZEXPECT_FALSE(req.empty());
    ZEXPECT_TRUE(req.find("\"rpc\"") != std::string::npos);
    ZEXPECT_TRUE(req.find("\"led\"") != std::string::npos);
    // 提取 rid（简化：固定回 1 —— sendRpc 首个 rid）
    ZEXPECT_TRUE(dev.sendJson(R"({"t":"resp","rid":1,"ok":true,"data":{"state":"on"}})"));

    for (int i = 0; i < 200 && !rpcDone.load(); ++i) std::this_thread::sleep_for(10ms);
    ZEXPECT_TRUE(rpcDone.load());
    ZEXPECT_TRUE(rpcResp.find("\"state\":\"on\"") != std::string::npos);

    // 事件（hello/hb）已被收集
    {
        std::lock_guard<std::mutex> lk(evMtx);
        ZEXPECT_TRUE(events.size() >= 2);
    }

    bridge->stop();
    mgr.unloadAll();
    ZEXPECT_EQ(registry.size(), size_t{0});
}

ZTEST_MAIN
