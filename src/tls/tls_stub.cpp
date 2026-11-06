// ============================================================================
//  TLS 空实现（-DZENITH_TLS=none）
//
//  背景：`ZENITH_TLS=none` 是文档里写着的合法配置，但此前没有任何 .cpp 提供
//  TlsFilter / OpenSslContext 的符号——不配 OpenSSL 构建时一律在链接期炸掉
//  （MSVC 任务第一次暴露）。这里给出「编译得过、运行期明确报错」的占位实现：
//  任何真正要用 TLS 的路径都会得到 "TLS backend not compiled" 而不是崩溃。
// ============================================================================
#if !defined(ZENITH_TLS_OPENSSL)

#include <utility>

#include "zenith/tls/tls_context.hpp"

namespace zenith::tls {
namespace {
constexpr int kNoTlsBackend = 90;
const char* kNoTlsMsg = "TLS backend not compiled (-DZENITH_TLS=none or OpenSSL not found)";
}  // namespace

void openSslGlobalInit() {}

core::Result<OpenSslContext> OpenSslContext::createServer(const TlsServerConfig&) {
    return core::Result<OpenSslContext>::Err(core::Error(kNoTlsBackend, kNoTlsMsg));
}

core::Result<OpenSslContext> OpenSslContext::createClient(const TlsClientConfig&) {
    return core::Result<OpenSslContext>::Err(core::Error(kNoTlsBackend, kNoTlsMsg));
}

OpenSslContext::~OpenSslContext() = default;

OpenSslContext::OpenSslContext(OpenSslContext&& other) noexcept
    : ctx_(other.ctx_), alpnHolder_(std::move(other.alpnHolder_)) {
    other.ctx_ = nullptr;
}

OpenSslContext& OpenSslContext::operator=(OpenSslContext&& other) noexcept {
    if (this != &other) {
        ctx_ = other.ctx_;
        alpnHolder_ = std::move(other.alpnHolder_);
        other.ctx_ = nullptr;
    }
    return *this;
}

SSL* OpenSslContext::newServerSession() const {
    return nullptr;
}

SSL* OpenSslContext::newClientSession(const std::string&) const {
    return nullptr;
}

TlsFilter::TlsFilter(bool /*serverSide*/, SSL* /*ssl*/) {
    fail(kNoTlsMsg);
}

TlsFilter::~TlsFilter() = default;

void TlsFilter::feedCipher(const char*, size_t, std::string&, std::string&) {
    fail(kNoTlsMsg);
}

void TlsFilter::encrypt(const char*, size_t, std::string&) {
    fail(kNoTlsMsg);
}

void TlsFilter::shutdown(std::string&) {}

void TlsFilter::pumpHandshake(std::string&) {}

void TlsFilter::drainRead(std::string&) {}

void TlsFilter::drainWire(std::string&) {}

void TlsFilter::fail(std::string msg) {
    state_ = TlsState::Failed;
    error_ = std::move(msg);
}

}  // namespace zenith::tls

#endif  // !ZENITH_TLS_OPENSSL
