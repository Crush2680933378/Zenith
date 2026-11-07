// ============================================================================
//  zenith::rtc::DtlsTransport —— WebRTC 的 DTLS 传输层（基于 OpenSSL）
//
//  WebRTC 的数据通道/媒体通道都跑在 DTLS 之上（SRTP 的密钥也由 DTLS 导出），
//  所以这一层是"能真正加密"的前提。
//
//  已做：DTLS 1.2 握手（服务端 DTLSv1_listen / 客户端 SSL_connect）、
//        记录收发、密钥导出（exportKeyingMaterial，SRTP 用）
//  未做：ICE 与 DTLS 的 socket 复用（当前各自持有一个 UDP socket）、
//        证书指纹协商（SDP 里的 fingerprint）、SRTP/SRTCP 本身
//
//  无 OpenSSL 构建下 `supported()` 返回 false，调用返回失败——不静默假装加密。
// ============================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "zenith/core/export.hpp"
#include "zenith/core/noncopyable.hpp"

namespace zenith::rtc {

class ZENITH_API DtlsTransport : private core::NonCopyable {
public:
    DtlsTransport();
    ~DtlsTransport();

    [[nodiscard]] static bool supported();

    /// 服务端：绑定 UDP 并载入证书
    bool listen(const std::string& certFile, const std::string& keyFile, uint16_t port = 0);

    /// 服务端：阻塞等待一个客户端完成握手
    bool accept(int timeoutMs = 5000);

    /// 客户端：与 host:port 完成握手（verifyPeer=false 时跳过证书校验）
    bool connect(const std::string& host, uint16_t port, bool verifyPeer = false,
                 int timeoutMs = 5000);

    [[nodiscard]] bool established() const;
    [[nodiscard]] uint16_t localPort() const;

    bool send(const std::string& data);
    /// 收一条记录；超时/错误返回空串，err 里给原因
    std::string receive(int timeoutMs = 2000, std::string* err = nullptr);

    /// 导出密钥材料（RFC 5705）；SRTP 就是用它派生会话密钥
    bool exportKeyingMaterial(const std::string& label, size_t len, std::vector<uint8_t>& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace zenith::rtc
