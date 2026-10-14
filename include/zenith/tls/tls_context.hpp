// ============================================================================
//  zenith::tls —— OpenSSL 后端（M4.1）
//  - OpenSslContext：证书/校验/SNI/ALPN 配置，产出 SSL* 会话
//  - TlsFilter：内存 BIO + 非阻塞握手状态机，作为 TcpConnection 的
//    加解密滤层——对 HttpServer/HttpClient 完全透明（密文进、明文出）
//  - M1 的 ITlsContext 契约（tls_transport.hpp）面向"自定义字节流传输"，
//    本文件是 Reactor 集成的具体后端 API；两者共存，后续按需桥接
//  构建开关：ZENITH_TLS=openssl（默认 none）
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "zenith/core/export.hpp"
#include "zenith/core/result.hpp"

// OpenSSL 前置声明（公共头不泄漏 openssl 类型）
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

namespace zenith::tls {

struct TlsServerConfig {
    std::string certFile;     // PEM 证书链（含中间证书）
    std::string keyFile;      // PEM 私钥
    std::string alpn = "";    // 空格分隔，如 "http/1.1"（可空）
};

struct TlsClientConfig {
    bool useSystemTrust = true; // SSL_CTX_set_default_verify_paths
    std::string caFile;         // 额外 CA 包（PEM）；空则仅系统信任
    bool verifyPeer = true;
};

/// 进程级 OpenSSL 初始化（幂等；首次创建 Context 时自动调用）
ZENITH_API void openSslGlobalInit();

class ZENITH_API OpenSslContext {
public:
    // ---- 工厂 ----
    static core::Result<OpenSslContext> createServer(const TlsServerConfig& cfg);
    static core::Result<OpenSslContext> createClient(const TlsClientConfig& cfg = {});

    OpenSslContext() = default;
    ~OpenSslContext();
    OpenSslContext(OpenSslContext&& other) noexcept;
    OpenSslContext& operator=(OpenSslContext&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return ctx_ != nullptr; }

    /// 新建 SSL 会话（服务端角色）。调用方负责 SSL_free（TlsFilter 接管）。
    [[nodiscard]] SSL* newServerSession() const;

    /// 新建 SSL 会话（客户端角色，含 SNI + 主机名校验）。
    [[nodiscard]] SSL* newClientSession(const std::string& serverName) const;

    [[nodiscard]] SSL_CTX* raw() const noexcept { return ctx_; }

private:
    friend class OpenSslContextFactory;

    explicit OpenSslContext(SSL_CTX* ctx) : ctx_(ctx) {}
    SSL_CTX* ctx_ = nullptr;
    std::shared_ptr<std::string> alpnHolder_; // ALPN 回调 arg 保活
};

/// TLS 握手/运行状态
enum class TlsState : uint8_t {
    Handshaking,
    Established,
    Failed,
    Closed,
};

/// 加解密滤层（TcpConnection 内部使用；一对内存 BIO 驱动）
class ZENITH_API TlsFilter {
public:
    /// serverSide：被动等握手；clientSide：构造后立即发起握手
    TlsFilter(bool serverSide, SSL* ssl);
    ~TlsFilter();
    TlsFilter(const TlsFilter&) = delete;
    TlsFilter& operator=(const TlsFilter&) = delete;

    /// 密文入站：喂入后推进握手/解密。
    /// 返回明文（可能为空）——追加到调用方缓冲。
    /// outWire：需要写出到 socket 的密文（握手记录 / 密文响应）。
    void feedCipher(const char* data, size_t n, std::string& outPlain,
                    std::string& outWire);

    /// 明文出站：加密。密文追加到 outWire；握手未完成时自动暂存并在完成后flush。
    void encrypt(const char* data, size_t n, std::string& outWire);

    /// 关闭：尽力发送 close_notify（不等待对端）。
    void shutdown(std::string& outWire);

    [[nodiscard]] TlsState state() const noexcept { return state_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    /// 是否刚完成握手（用于触发 onConnect 一次性事件）
    bool consumeHandshakeDone() {
        const bool d = handshakeDone_;
        handshakeDone_ = false;
        return d;
    }

private:
    void pumpHandshake(std::string& outWire);
    void drainRead(std::string& outPlain);
    void drainWire(std::string& outWire);
    void fail(std::string msg);
    [[nodiscard]] bool established() const noexcept { return state_ == TlsState::Established; }

    SSL* ssl_ = nullptr;
    void* rbio_ = nullptr; // BIO*（公共头不泄漏类型）
    void* wbio_ = nullptr;
    TlsState state_ = TlsState::Handshaking;
    bool handshakeDone_ = false;
    bool shutdownSent_ = false;
    std::string pendingPlain_;          // 握手期间暂存的明文
    std::string pendingWireAtConstruct_; // 客户端构造时产生的 ClientHello
    std::string error_;
};

} // namespace zenith::tls
