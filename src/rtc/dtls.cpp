// ============================================================================
//  DTLS 传输层实现（OpenSSL）
// ============================================================================
#include "zenith/rtc/dtls.hpp"

#include "zenith/tls/tls_context.hpp"

#if defined(ZENITH_TLS_OPENSSL)

#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#if defined(_WIN32)
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

#include <chrono>
#include <cstring>

namespace zenith::rtc {
namespace {

void closeSock(int fd) {
    if (fd < 0) return;
#if defined(_WIN32)
    closesocket(fd);
#else
    ::close(fd);
#endif
}

void setRecvTimeout(int fd, int timeoutMs) {
#if defined(_WIN32)
    DWORD tv = static_cast<DWORD>(timeoutMs);
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

/// DTLSv1_listen 要求 cookie 回调（防 DoS 的 HelloVerifyRequest 流程）。
/// 这里用随机 cookie + 无条件校验：够跑通握手，生产应改成 HMAC(对端地址 + 轮换密钥)。
int dtlsCookieGenerate(SSL* ssl, unsigned char* cookie, unsigned int* cookieLen) {
    (void)ssl;
    constexpr unsigned kCookieLen = 16;
    if (RAND_bytes(cookie, static_cast<int>(kCookieLen)) != 1) return 0;
    *cookieLen = kCookieLen;
    return 1;
}

int dtlsCookieVerify(SSL* ssl, const unsigned char* cookie, unsigned int cookieLen) {
    (void)ssl;
    (void)cookie;
    (void)cookieLen;
    return 1;  // 简化：任何回显的 cookie 都接受
}

std::string lastError(const char* what) {
    const unsigned long e = ERR_get_error();
    if (e == 0) return what;
    char buf[256] = {};
    ERR_error_string_n(e, buf, sizeof(buf));
    return std::string(what) + ": " + buf;
}

}  // namespace

struct DtlsTransport::Impl {
    SSL_CTX* ctx = nullptr;
    SSL* ssl = nullptr;
    int fd = -1;
    uint16_t port = 0;
    bool server = false;
    bool established = false;

    ~Impl() { reset(); }

    void reset() {
        if (ssl != nullptr) {
            SSL_free(ssl);
            ssl = nullptr;
        }
        if (ctx != nullptr) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        closeSock(fd);
        fd = -1;
        established = false;
    }

    static int bindUdp(const std::string& host, uint16_t port, uint16_t& outPort) {
#if defined(_WIN32)
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
        const int fd = static_cast<int>(::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP));
        if (fd < 0) return -1;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (host.empty()) {
            addr.sin_addr.s_addr = INADDR_ANY;
        } else if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
            closeSock(fd);
            return -1;
        }
        if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            closeSock(fd);
            return -1;
        }
        socklen_t len = sizeof(addr);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) == 0) {
            outPort = ntohs(addr.sin_port);
        }
        return fd;
    }
};

DtlsTransport::DtlsTransport() : impl_(std::make_unique<Impl>()) {}
DtlsTransport::~DtlsTransport() = default;

bool DtlsTransport::supported() {
    return true;
}

bool DtlsTransport::listen(const std::string& certFile, const std::string& keyFile, uint16_t port) {
    impl_->reset();
    tls::openSslGlobalInit();
    impl_->ctx = SSL_CTX_new(DTLS_server_method());
    if (impl_->ctx == nullptr) return false;
    if (SSL_CTX_use_certificate_chain_file(impl_->ctx, certFile.c_str()) != 1) {
        impl_->reset();
        return false;
    }
    if (SSL_CTX_use_PrivateKey_file(impl_->ctx, keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        impl_->reset();
        return false;
    }
    // WebRTC 用自签证书 + 指纹校验，这里不强制对端 CA
    SSL_CTX_set_verify(impl_->ctx, SSL_VERIFY_NONE, nullptr);
    SSL_CTX_set_read_ahead(impl_->ctx, 1);
    // DTLSv1_listen 必需：没有 cookie 回调会直接报 "cookie gen callback failure"
    SSL_CTX_set_cookie_generate_cb(impl_->ctx, dtlsCookieGenerate);
    SSL_CTX_set_cookie_verify_cb(impl_->ctx, dtlsCookieVerify);

    impl_->fd = Impl::bindUdp("127.0.0.1", port, impl_->port);
    if (impl_->fd < 0) {
        impl_->reset();
        return false;
    }
    impl_->server = true;
    return true;
}

bool DtlsTransport::accept(int timeoutMs) {
    if (impl_->fd < 0 || impl_->ctx == nullptr) return false;
    impl_->ssl = SSL_new(impl_->ctx);
    if (impl_->ssl == nullptr) return false;

    BIO* bio = BIO_new_dgram(impl_->fd, BIO_NOCLOSE);
    if (bio == nullptr) return false;
    SSL_set_bio(impl_->ssl, bio, bio);
    SSL_set_options(impl_->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(impl_->ssl, 1200);

    // 等第一个 ClientHello，拿到对端地址后把 socket 连过去
    setRecvTimeout(impl_->fd, timeoutMs);
    BIO_ADDR* clientAddr = BIO_ADDR_new();
    const int rc = DTLSv1_listen(impl_->ssl, clientAddr);
    bool ok = false;
    if (rc > 0) {
        sockaddr_in peer{};
        if (BIO_ADDR_family(clientAddr) == AF_INET) {
            peer.sin_family = AF_INET;
            BIO_ADDR_rawaddress(clientAddr, &peer.sin_addr, nullptr);
            peer.sin_port = BIO_ADDR_rawport(clientAddr);
            if (::connect(impl_->fd, reinterpret_cast<sockaddr*>(&peer), sizeof(peer)) == 0) {
                BIO_ctrl(SSL_get_rbio(impl_->ssl), BIO_CTRL_DGRAM_SET_CONNECTED, 0, &peer);
                setRecvTimeout(impl_->fd, timeoutMs);
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
                while (std::chrono::steady_clock::now() < deadline) {
                    const int arc = SSL_accept(impl_->ssl);
                    if (arc == 1) {
                        ok = true;
                        break;
                    }
                    const int e = SSL_get_error(impl_->ssl, arc);
                    if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
                        break;
                    }
                }
            }
        }
    }
    BIO_ADDR_free(clientAddr);
    impl_->established = ok;
    return ok;
}

bool DtlsTransport::connect(const std::string& host, uint16_t port, bool verifyPeer,
                            int timeoutMs) {
    impl_->reset();
    tls::openSslGlobalInit();
    impl_->ctx = SSL_CTX_new(DTLS_client_method());
    if (impl_->ctx == nullptr) return false;
    SSL_CTX_set_verify(impl_->ctx, verifyPeer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);

    impl_->fd = Impl::bindUdp("127.0.0.1", 0, impl_->port);
    if (impl_->fd < 0) {
        impl_->reset();
        return false;
    }
    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &server.sin_addr) != 1) {
        impl_->reset();
        return false;
    }
    if (::connect(impl_->fd, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0) {
        impl_->reset();
        return false;
    }

    impl_->ssl = SSL_new(impl_->ctx);
    if (impl_->ssl == nullptr) {
        impl_->reset();
        return false;
    }
    BIO* bio = BIO_new_dgram(impl_->fd, BIO_NOCLOSE);
    if (bio == nullptr) {
        impl_->reset();
        return false;
    }
    // DTLS 需要知道对端地址（连接型 UDP socket 也得显式告诉 BIO）
    ::connect(impl_->fd, reinterpret_cast<sockaddr*>(&server), sizeof(server));
    BIO_ctrl(bio, BIO_CTRL_DGRAM_SET_CONNECTED, 0, &server);
    SSL_set_bio(impl_->ssl, bio, bio);
    SSL_set_options(impl_->ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(impl_->ssl, 1200);
    setRecvTimeout(impl_->fd, timeoutMs);
    // DTLS 首包可能丢（UDP），必须按 WANT_READ 重试直到超时，而不是一次失败即放弃
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const int rc = SSL_connect(impl_->ssl);
        if (rc == 1) {
            ok = true;
            break;
        }
        const int e = SSL_get_error(impl_->ssl, rc);
        if (e != SSL_ERROR_WANT_READ && e != SSL_ERROR_WANT_WRITE) {
            break;
        }
    }
    impl_->established = ok;
    return ok;
}

bool DtlsTransport::established() const {
    return impl_->established;
}
uint16_t DtlsTransport::localPort() const {
    return impl_->port;
}

bool DtlsTransport::send(const std::string& data) {
    if (!impl_->established || data.empty()) return false;
    const int n = SSL_write(impl_->ssl, data.data(), static_cast<int>(data.size()));
    return n == static_cast<int>(data.size());
}

std::string DtlsTransport::receive(int timeoutMs, std::string* err) {
    if (!impl_->established) {
        if (err) *err = "not established";
        return {};
    }
    setRecvTimeout(impl_->fd, timeoutMs);
    char buf[2048];
    const int n = SSL_read(impl_->ssl, buf, sizeof(buf));
    if (n <= 0) {
        if (err != nullptr) {
            const int e = SSL_get_error(impl_->ssl, n);
            *err = (e == SSL_ERROR_WANT_READ || e == SSL_ERROR_SYSCALL) ? "timeout"
                                                                        : lastError("SSL_read");
        }
        return {};
    }
    return std::string(buf, static_cast<size_t>(n));
}

bool DtlsTransport::exportKeyingMaterial(const std::string& label, size_t len,
                                         std::vector<uint8_t>& out) {
    if (!impl_->established) return false;
    out.assign(len, 0);
    return SSL_export_keying_material(impl_->ssl, out.data(), out.size(), label.c_str(),
                                      label.size(), nullptr, 0, 0) == 1;
}

}  // namespace zenith::rtc

#else  // 无 OpenSSL：桩实现

namespace zenith::rtc {

struct DtlsTransport::Impl {};

DtlsTransport::DtlsTransport() : impl_(std::make_unique<Impl>()) {}
DtlsTransport::~DtlsTransport() = default;

bool DtlsTransport::supported() {
    return false;
}
bool DtlsTransport::listen(const std::string&, const std::string&, uint16_t) {
    return false;
}
bool DtlsTransport::accept(int) {
    return false;
}
bool DtlsTransport::connect(const std::string&, uint16_t, bool, int) {
    return false;
}
bool DtlsTransport::established() const {
    return false;
}
uint16_t DtlsTransport::localPort() const {
    return 0;
}
bool DtlsTransport::send(const std::string&) {
    return false;
}
std::string DtlsTransport::receive(int, std::string* err) {
    if (err != nullptr) *err = "DTLS backend not compiled (no OpenSSL)";
    return {};
}
bool DtlsTransport::exportKeyingMaterial(const std::string&, size_t, std::vector<uint8_t>&) {
    return false;
}

}  // namespace zenith::rtc

#endif
