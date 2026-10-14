#include "zenith/tls/tls_context.hpp"

#include <mutex>

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>

namespace zenith::tls {

void openSslGlobalInit() {
    static std::once_flag once;
    std::call_once(once, [] {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS,
                         nullptr);
    });
}

namespace {
std::string lastSslError(const char* context) {
    const unsigned long code = ERR_get_error();
    char buf[256];
    if (code == 0) {
        return std::string(context) + ": unknown openssl error";
    }
    ERR_error_string_n(code, buf, sizeof(buf));
    return std::string(context) + ": " + buf;
}
} // namespace

/* ==========================================================================
 * OpenSslContext
 * ======================================================================== */

OpenSslContext::~OpenSslContext() {
    if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

OpenSslContext::OpenSslContext(OpenSslContext&& other) noexcept : ctx_(other.ctx_) {
    other.ctx_ = nullptr;
}

OpenSslContext& OpenSslContext::operator=(OpenSslContext&& other) noexcept {
    if (this != &other) {
        if (ctx_ != nullptr) SSL_CTX_free(ctx_);
        ctx_ = std::exchange(other.ctx_, nullptr);
    }
    return *this;
}

core::Result<OpenSslContext> OpenSslContext::createServer(const TlsServerConfig& cfg) {
    openSslGlobalInit();
    SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
    if (ctx == nullptr) {
        return core::Result<OpenSslContext>::Err(core::Error(80, lastSslError("SSL_CTX_new")));
    }
    // 现代安全基线：TLS1.2+，禁压缩/重协商
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION | SSL_OP_NO_RENEGOTIATION);

    if (SSL_CTX_use_certificate_chain_file(ctx, cfg.certFile.c_str()) != 1) {
        SSL_CTX_free(ctx);
        return core::Result<OpenSslContext>::Err(
            core::Error(81, lastSslError(("load cert " + cfg.certFile).c_str())));
    }
    if (SSL_CTX_use_PrivateKey_file(ctx, cfg.keyFile.c_str(), SSL_FILETYPE_PEM) != 1) {
        SSL_CTX_free(ctx);
        return core::Result<OpenSslContext>::Err(
            core::Error(82, lastSslError(("load key " + cfg.keyFile).c_str())));
    }
    if (SSL_CTX_check_private_key(ctx) != 1) {
        SSL_CTX_free(ctx);
        return core::Result<OpenSslContext>::Err(
            core::Error(83, "private key does not match certificate"));
    }
    OpenSslContext out(ctx);
    if (!cfg.alpn.empty()) {
        // "http/1.1" → "\x08http/1.1"（长度前缀串）；holder 与 Context 同寿命保活
        auto wire = std::make_shared<std::string>();
        size_t begin = 0;
        while (begin <= cfg.alpn.size()) {
            const size_t sp = cfg.alpn.find(' ', begin);
            const std::string proto =
                cfg.alpn.substr(begin, sp == std::string::npos ? std::string::npos : sp - begin);
            if (!proto.empty() && proto.size() < 256) {
                wire->push_back(static_cast<char>(proto.size()));
                *wire += proto;
            }
            if (sp == std::string::npos) break;
            begin = sp + 1;
        }
        SSL_CTX_set_alpn_select_cb(
            ctx,
            [](SSL*, const unsigned char** out, unsigned char* outLen, const unsigned char* in,
               unsigned int inLen, void* arg) {
                const std::string* wire = static_cast<const std::string*>(arg);
                if (SSL_select_next_proto(const_cast<unsigned char**>(out), outLen,
                                          reinterpret_cast<const unsigned char*>(wire->data()),
                                          static_cast<unsigned int>(wire->size()), in,
                                          inLen) != OPENSSL_NPN_NEGOTIATED) {
                    return SSL_TLSEXT_ERR_NOACK;
                }
                return SSL_TLSEXT_ERR_OK;
            },
            wire.get());
        out.alpnHolder_ = wire;
    }
    return core::Result<OpenSslContext>::Ok(std::move(out));
}

core::Result<OpenSslContext> OpenSslContext::createClient(const TlsClientConfig& cfg) {
    openSslGlobalInit();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (ctx == nullptr) {
        return core::Result<OpenSslContext>::Err(core::Error(80, lastSslError("SSL_CTX_new")));
    }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_options(ctx, SSL_OP_NO_COMPRESSION);

    if (cfg.useSystemTrust) {
        SSL_CTX_set_default_verify_paths(ctx);
    }
    if (!cfg.caFile.empty() &&
        SSL_CTX_load_verify_locations(ctx, cfg.caFile.c_str(), nullptr) != 1) {
        SSL_CTX_free(ctx);
        return core::Result<OpenSslContext>::Err(
            core::Error(84, lastSslError(("load ca " + cfg.caFile).c_str())));
    }
    if (cfg.verifyPeer) {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    }
    return core::Result<OpenSslContext>::Ok(OpenSslContext(ctx));
}

SSL* OpenSslContext::newServerSession() const {
    if (ctx_ == nullptr) return nullptr;
    return SSL_new(ctx_);
}

SSL* OpenSslContext::newClientSession(const std::string& serverName) const {
    if (ctx_ == nullptr) return nullptr;
    SSL* ssl = SSL_new(ctx_);
    if (ssl == nullptr) return nullptr;
    if (!serverName.empty()) {
        SSL_set_tlsext_host_name(ssl, serverName.c_str()); // SNI
        SSL_set1_host(ssl, serverName.c_str());            // 证书主机名校验
    }
    return ssl;
}

/* ==========================================================================
 * TlsFilter（内存 BIO 驱动）
 * ======================================================================== */

TlsFilter::TlsFilter(bool serverSide, SSL* ssl) : ssl_(ssl) {
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    SSL_set_bio(ssl_, static_cast<BIO*>(rbio_), static_cast<BIO*>(wbio_));
    if (serverSide) {
        SSL_set_accept_state(ssl_);
    } else {
        SSL_set_connect_state(ssl_);
        // 客户端：构造即发起握手（pump 由首条出站或入站触发）
        std::string wire;
        pumpHandshake(wire);
        pendingWireAtConstruct_ = wire;
    }
}

TlsFilter::~TlsFilter() {
    if (ssl_ != nullptr) SSL_free(ssl_); // 同时释放关联 BIO
}

void TlsFilter::feedCipher(const char* data, size_t n, std::string& outPlain,
                            std::string& outWire) {
    if (state_ == TlsState::Failed || state_ == TlsState::Closed) return;
    if (!pendingWireAtConstruct_.empty()) {
        outWire += pendingWireAtConstruct_; // 客户端首条 ClientHello
        pendingWireAtConstruct_.clear();
    }
    if (BIO_write(static_cast<BIO*>(rbio_), data, static_cast<int>(n)) < 0) {
        fail("BIO_write failed");
        return;
    }
    if (!established()) {
        pumpHandshake(outWire);
        if (!established()) return;
    }
    drainRead(outPlain);
}

void TlsFilter::encrypt(const char* data, size_t n, std::string& outWire) {
    if (state_ == TlsState::Failed || state_ == TlsState::Closed) return;
    if (!pendingWireAtConstruct_.empty()) {
        outWire += pendingWireAtConstruct_;
        pendingWireAtConstruct_.clear();
    }
    if (n == 0) {
        if (!established()) pumpHandshake(outWire); // 仅推进握手
        return;
    }
    if (!established()) {
        pendingPlain_.append(data, n); // 握手未完：暂存
        pumpHandshake(outWire);
        return;
    }
    int written = SSL_write(ssl_, data, static_cast<int>(n));
    if (written <= 0) {
        fail(lastSslError("SSL_write"));
        return;
    }
    drainWire(outWire);
}

void TlsFilter::shutdown(std::string& outWire) {
    if (shutdownSent_ || ssl_ == nullptr) return;
    shutdownSent_ = true;
    if (established()) {
        SSL_shutdown(ssl_);
        drainWire(outWire);
    }
    state_ = TlsState::Closed;
}

void TlsFilter::pumpHandshake(std::string& outWire) {
    for (int round = 0; round < 8; ++round) { // 防意外死循环
        if (SSL_is_init_finished(ssl_)) {
            state_ = TlsState::Established;
            handshakeDone_ = true;
            // 握手完成：冲刷暂存明文
            if (!pendingPlain_.empty()) {
                std::string pending = std::move(pendingPlain_);
                pendingPlain_.clear();
                std::string wire;
                encrypt(pending.data(), pending.size(), wire);
                outWire += wire;
            }
            return;
        }
        const int r = SSL_do_handshake(ssl_);
        drainWire(outWire);
        if (r == 1) {
            state_ = TlsState::Established;
            handshakeDone_ = true;
            if (!pendingPlain_.empty()) {
                std::string pending = std::move(pendingPlain_);
                pendingPlain_.clear();
                std::string wire;
                encrypt(pending.data(), pending.size(), wire);
                outWire += wire;
            }
            return;
        }
        const int err = SSL_get_error(ssl_, r);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            continue; // 正常等待对端数据（read 走 rbio）
        }
        fail(lastSslError("handshake"));
        return;
    }
}

void TlsFilter::drainRead(std::string& outPlain) {
    char buf[16384];
    for (;;) {
        const int n = SSL_read(ssl_, buf, sizeof(buf));
        if (n > 0) {
            outPlain.append(buf, static_cast<size_t>(n));
            continue;
        }
        const int err = SSL_get_error(ssl_, n);
        if (err == SSL_ERROR_WANT_READ) return; // 读尽
        if (err == SSL_ERROR_ZERO_RETURN) {     // 对端 close_notify
            state_ = TlsState::Closed;
            return;
        }
        fail(lastSslError("SSL_read"));
        return;
    }
}

void TlsFilter::drainWire(std::string& outWire) {
    char buf[16384];
    for (;;) {
        const int n = BIO_read(static_cast<BIO*>(wbio_), buf, sizeof(buf));
        if (n > 0) {
            outWire.append(buf, static_cast<size_t>(n));
            continue;
        }
        return; // 读尽（BIO EOF）
    }
}

void TlsFilter::fail(std::string msg) {
    if (state_ == TlsState::Established) state_ = TlsState::Failed;
    else state_ = TlsState::Failed;
    error_ = std::move(msg);
}

} // namespace zenith::tls
