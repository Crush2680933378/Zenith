// ============================================================================
//  ITlsContext / ITlsSession —— TLS 传输抽象接口（M1 仅契约，M2/M3 实装）
//
//  后端策略（CMake 选项 ZENITH_TLS，默认 none）：
//    openssl  —— 桌面 / 服务器场景（libssl，纯 C ABI，直接链接）
//    mbedtls  —— 嵌入式场景（RK3566/RK3576、轻量设备）
//    wolfssl  —— 嵌入式 / FIPS 场景（含 ESP32 适配）
//    none     —— 不编 TLS（纯内网 / 明文部署）
//
//  设计要点：
//    - 后端以编译期选择 + 工厂函数注册进 ServiceRegistry，业务代码只
//      依赖本抽象；后端动态热插拔（作为插件）留待 M4 评估。
//    - 会话面向「字节流收发回调」而非直接绑定 socket：同一段代码即可
//      跑在 TCP、串口转发、甚至内存管道之上（配合 edge 插件使用）。
// ============================================================================
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "zenith/core/result.hpp"
#include "zenith/plugin/service.hpp"

namespace zenith::tls {

/// 握手角色。
enum class TlsRole : uint8_t {
    Client,
    Server,
};

/// 单次收发回调返回值：>0 成功字节数；0 对端关闭；<0 错误。
using IoCallback = int32_t (*)(void* ioCtx, uint8_t* buf, size_t cap);

/// TLS 配置（与后端无关的最小公共集；后端特有项经 ITlsContext::setOption 扩展）。
struct TlsConfig {
    TlsRole     role       = TlsRole::Client;
    bool        verifyPeer = true;   // 客户端校验服务端证书
    const char* caFile     = nullptr;
    const char* certFile   = nullptr;
    const char* keyFile    = nullptr;
    const char* serverName = nullptr; // SNI
};

class ITlsSession {
public:
    virtual ~ITlsSession() = default;

    /// 非阻塞握手：返回 Done / WantRead / WantWrite / 错误。
    enum class HandshakeState : uint8_t { Done, WantRead, WantWrite, Failed };

    virtual HandshakeState handshake() = 0;

    /// 加密发送明文。返回成功消费的明文字节数（<0 错误）。
    virtual int32_t encrypt(const uint8_t* plaintext, size_t len) = 0;

    /// 解密接收（写入 out，返回写出字节数；0 = 暂无数据；<0 错误）。
    virtual int32_t decrypt(uint8_t* out, size_t cap) = 0;

    /// 优雅关闭（close_notify）。
    virtual void shutdownTls() = 0;
};

class ITlsContext : public plugin::IService {
public:
    ZENITH_SERVICE_DECL(ITlsContext, "zenith.tls.ITlsContext", 1)

    /// 应用配置并创建会话。ioRead/ioWrite 由传输层提供（TCP / 管道 / 串口桥）。
    virtual core::Result<std::unique_ptr<ITlsSession>>
    createSession(const TlsConfig& cfg, IoCallback ioRead, IoCallback ioWrite,
                  void* ioCtx) = 0;

    /// 后端特有选项（透传 key/value，如 OpenSSL 的 "min_proto=TLS1.2"）。
    virtual core::Result<void> setOption(const char* key, const char* value) = 0;
};

} // namespace zenith::tls
