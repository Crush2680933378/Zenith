// ============================================================================
//  zenith::http::jwt::asym —— 非对称 JWT（M9.2，RS256/384/512、ES256/384）
//  仅 OpenSSL 后端编入。用于对接第三方 IdP、Passkey、ACME 等需要公钥验签的场景。
//  纯 C ABI 依赖（OpenSSL），符合 README 依赖原则。
// ============================================================================
#include "zenith/http/jwt.hpp"

#if defined(ZENITH_HAS_OPENSSL)

#include <openssl/bio.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace zenith::http::jwt::asym {

namespace {

using PkeyPtr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;

struct MdCtx {
    EVP_MD_CTX* p = EVP_MD_CTX_new();
    ~MdCtx() { EVP_MD_CTX_free(p); }
    MdCtx() = default;
    MdCtx(const MdCtx&) = delete;
    MdCtx& operator=(const MdCtx&) = delete;
    [[nodiscard]] bool ok() const noexcept { return p != nullptr; }
};

// ---- PEM 读写 --------------------------------------------------------------
PkeyPtr readPrivate(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) return PkeyPtr(nullptr, EVP_PKEY_free);
    EVP_PKEY* key = PEM_read_bio_PrivateKey(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return PkeyPtr(key, EVP_PKEY_free);
}

PkeyPtr readPublic(const std::string& pem) {
    BIO* bio = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    if (bio == nullptr) return PkeyPtr(nullptr, EVP_PKEY_free);
    EVP_PKEY* key = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    BIO_free(bio);
    return PkeyPtr(key, EVP_PKEY_free);
}

std::string bioToString(BIO* bio) {
    char* data = nullptr;
    const long n = BIO_get_mem_data(bio, &data);
    return (n > 0 && data != nullptr) ? std::string(data, static_cast<size_t>(n)) : std::string();
}

std::string writePrivatePem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) return {};
    const bool ok = PEM_write_bio_PKCS8PrivateKey(bio, key, nullptr, nullptr, 0, nullptr,
                                                  nullptr) == 1;
    const std::string out = ok ? bioToString(bio) : std::string();
    BIO_free(bio);
    return out;
}

std::string writePublicPem(EVP_PKEY* key) {
    BIO* bio = BIO_new(BIO_s_mem());
    if (bio == nullptr) return {};
    const bool ok = PEM_write_bio_PUBKEY(bio, key) == 1;
    const std::string out = ok ? bioToString(bio) : std::string();
    BIO_free(bio);
    return out;
}

const char* ecCurveAlg(EVP_PKEY* key) {
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    char name[80] = {0};
    size_t len = 0;
    if (EVP_PKEY_get_group_name(key, name, sizeof(name) - 1, &len) != 1) return nullptr;
    const std::string group(name, len);
    if (group == "prime256v1" || group == "P-256" || group == "secp256r1") return "ES256";
    if (group == "secp384r1" || group == "P-384") return "ES384";
    return nullptr;
#else
    const EC_KEY* ec = EVP_PKEY_get0_EC_KEY(key);
    if (ec == nullptr) return nullptr;
    const int nid = EC_GROUP_get_curve_name(EC_KEY_get0_group(ec));
    if (nid == NID_X9_62_prime256v1) return "ES256";
    if (nid == NID_secp384r1) return "ES384";
    return nullptr;
#endif
}

/// 私钥 → 算法名（不支持返回 nullptr）
const char* algForPrivateKey(EVP_PKEY* key) {
    switch (EVP_PKEY_base_id(key)) {
        case EVP_PKEY_RSA: return "RS256";
        case EVP_PKEY_EC: return ecCurveAlg(key);
        default: return nullptr;
    }
}

const EVP_MD* mdFor(const std::string& alg) {
    if (alg == "RS256" || alg == "ES256") return EVP_sha256();
    if (alg == "RS384" || alg == "ES384") return EVP_sha384();
    if (alg == "RS512") return EVP_sha512();
    if (alg == "PS256") return EVP_sha256();
    return nullptr;
}

/// ECDSA 签名的 R||S 分量长度（ES256=32、ES384=48、ES512=66）
size_t fieldLenFor(const std::string& alg) {
    if (alg == "ES256") return 32;
    if (alg == "ES384") return 48;
    if (alg == "ES512") return 66;
    return 0;
}

// ---- DER ↔ R||S（JWS 的 ECDSA 签名是定长拼接，OpenSSL 给的是 DER 序列）----
bool readDerLength(const uint8_t* der, size_t derLen, size_t& pos, size_t& len) {
    if (pos >= derLen) return false;
    const uint8_t first = der[pos++];
    if ((first & 0x80) == 0) {
        len = first;
        return true;
    }
    const size_t n = first & 0x7F;
    if (n == 0 || n > 2 || pos + n > derLen) return false;
    len = 0;
    for (size_t i = 0; i < n; ++i) len = (len << 8) | der[pos++];
    return true;
}

bool derToRaw(const uint8_t* der, size_t derLen, size_t fieldLen, std::vector<uint8_t>& out) {
    size_t pos = 0;
    size_t seqLen = 0;
    if (pos >= derLen || der[pos++] != 0x30 || !readDerLength(der, derLen, pos, seqLen)) return false;
    std::vector<uint8_t> part[2];
    for (int k = 0; k < 2; ++k) {
        size_t len = 0;
        if (pos >= derLen || der[pos++] != 0x02 || !readDerLength(der, derLen, pos, len)) return false;
        if (pos + len > derLen) return false;
        const uint8_t* p = der + pos;
        pos += len;
        while (len > 1 && p[0] == 0x00) { // 去 DER 前导零
            ++p;
            --len;
        }
        part[k].assign(p, p + len);
    }
    out.assign(fieldLen * 2, 0);
    for (int k = 0; k < 2; ++k) {
        if (part[k].size() > fieldLen) return false;
        std::copy(part[k].begin(), part[k].end(),
                  out.begin() + static_cast<long>(k * fieldLen + (fieldLen - part[k].size())));
    }
    return true;
}

std::vector<uint8_t> rawToDer(const uint8_t* raw, size_t fieldLen) {
    std::vector<uint8_t> body;
    body.reserve(fieldLen * 2 + 8);
    for (int k = 0; k < 2; ++k) {
        const uint8_t* p = raw + k * fieldLen;
        size_t n = fieldLen;
        while (n > 1 && p[0] == 0x00) {
            ++p;
            --n;
        }
        body.push_back(0x02);
        if ((p[0] & 0x80) != 0) { // 最高位为 1 → 补 0 视为正数
            body.push_back(static_cast<uint8_t>(n + 1));
            body.push_back(0x00);
        } else {
            body.push_back(static_cast<uint8_t>(n));
        }
        body.insert(body.end(), p, p + n);
    }
    std::vector<uint8_t> out;
    out.push_back(0x30);
    if (body.size() < 128) {
        out.push_back(static_cast<uint8_t>(body.size()));
    } else {
        out.push_back(0x81);
        out.push_back(static_cast<uint8_t>(body.size()));
    }
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

/// payload 注入 exp（与 HS256 路径同款文本注入，payload 必须是 JSON 对象）
std::string injectExp(const std::string& payloadJson, int64_t expAbs) {
    const std::string exp = "\"exp\":" + std::to_string(expAbs);
    std::string payload = payloadJson;
    if (payload.size() >= 2 && payload.back() == '}') {
        payload.insert(payload.size() - 1, payload.size() > 2 ? ("," + exp) : exp);
    } else {
        payload = "{" + exp + "}";
    }
    return payload;
}

core::Result<std::string> err(std::string msg) {
    return core::Result<std::string>::Err(core::Error(400, std::move(msg)));
}

std::string b64url(const std::string& s) { return detail::b64url(s); }
std::string b64url(const uint8_t* d, size_t n) { return detail::b64url(d, n); }

} // namespace

// ---- 密钥生成 --------------------------------------------------------------
core::Result<KeyPair> generateRsa(int bits) {
    if (bits < 2048) return core::Result<KeyPair>::Err(core::Error(400, "RSA bits must be >= 2048"));
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (ctx == nullptr) return core::Result<KeyPair>::Err(core::Error(500, "EVP_PKEY_CTX_new_id failed"));
    EVP_PKEY* raw = nullptr;
    bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
              EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, bits) == 1 &&
              EVP_PKEY_keygen(ctx, &raw) == 1;
    EVP_PKEY_CTX_free(ctx);
    PkeyPtr key(raw, EVP_PKEY_free);
    if (!ok || key == nullptr) return core::Result<KeyPair>::Err(core::Error(500, "RSA keygen failed"));

    KeyPair kp;
    kp.privatePem = writePrivatePem(key.get());
    kp.publicPem = writePublicPem(key.get());
    if (kp.privatePem.empty() || kp.publicPem.empty()) {
        return core::Result<KeyPair>::Err(core::Error(500, "PEM export failed"));
    }
    return core::Result<KeyPair>::Ok(std::move(kp));
}

core::Result<KeyPair> generateEcP256() {
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
    if (ctx == nullptr) return core::Result<KeyPair>::Err(core::Error(500, "EVP_PKEY_CTX_new_id failed"));
    EVP_PKEY* raw = nullptr;
    const bool ok = EVP_PKEY_keygen_init(ctx) == 1 &&
                    EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx, NID_X9_62_prime256v1) == 1 &&
                    EVP_PKEY_keygen(ctx, &raw) == 1;
    EVP_PKEY_CTX_free(ctx);
    PkeyPtr key(raw, EVP_PKEY_free);
    if (!ok || key == nullptr) return core::Result<KeyPair>::Err(core::Error(500, "EC keygen failed"));

    KeyPair kp;
    kp.privatePem = writePrivatePem(key.get());
    kp.publicPem = writePublicPem(key.get());
    if (kp.privatePem.empty() || kp.publicPem.empty()) {
        return core::Result<KeyPair>::Err(core::Error(500, "PEM export failed"));
    }
    return core::Result<KeyPair>::Ok(std::move(kp));
}

// ---- 签发 ------------------------------------------------------------------
core::Result<std::string> sign(const std::string& privatePem, const std::string& payloadJson,
                               int64_t expiresInSeconds, int64_t nowSec) {
    PkeyPtr key = readPrivate(privatePem);
    if (key == nullptr) return err("invalid private key PEM");

    const char* algName = algForPrivateKey(key.get());
    if (algName == nullptr) return err("unsupported key type for JWT signing");
    const std::string alg(algName);
    const EVP_MD* md = mdFor(alg);
    if (md == nullptr) return err("unsupported alg");

    const std::string payload = injectExp(payloadJson, nowSec + expiresInSeconds);
    const std::string signingInput =
        b64url(std::string("{\"alg\":\"") + alg + "\",\"typ\":\"JWT\"}") + "." + b64url(payload);

    MdCtx ctx;
    if (!ctx.ok()) return err("EVP_MD_CTX_new failed");
    if (EVP_DigestSignInit(ctx.p, nullptr, md, nullptr, key.get()) != 1) return err("sign init failed");
    if (EVP_DigestSignUpdate(ctx.p, signingInput.data(), signingInput.size()) != 1) {
        return err("sign update failed");
    }
    size_t sigLen = 0;
    if (EVP_DigestSignFinal(ctx.p, nullptr, &sigLen) != 1) return err("sign size failed");
    std::vector<uint8_t> sig(sigLen);
    if (EVP_DigestSignFinal(ctx.p, sig.data(), &sigLen) != 1) return err("sign failed");
    sig.resize(sigLen);

    const size_t fieldLen = fieldLenFor(alg);
    if (fieldLen != 0) { // ECDSA：DER → R||S 定长
        std::vector<uint8_t> rawSig;
        if (!derToRaw(sig.data(), sig.size(), fieldLen, rawSig)) return err("ECDSA DER decode failed");
        sig = std::move(rawSig);
    }
    return core::Result<std::string>::Ok(signingInput + "." + b64url(sig.data(), sig.size()));
}

core::Result<std::string> sign(const KeyPair& kp, const std::string& payloadJson,
                               int64_t expiresInSeconds, int64_t nowSec) {
    return sign(kp.privatePem, payloadJson, expiresInSeconds, nowSec);
}

// ---- 校验 ------------------------------------------------------------------
VerifyResult verify(const std::string& publicPem, const std::string& token, int64_t nowSec,
                    int64_t leewaySec) {
    VerifyResult r;
    const size_t d1 = token.find('.');
    const size_t d2 = token.rfind('.');
    if (d1 == std::string::npos || d2 == d1) {
        r.error = "malformed token";
        return r;
    }

    const std::string alg = headerAlg(token);
    if (alg.empty()) {
        r.error = "missing alg";
        return r;
    }
    if (alg == "none" || alg.compare(0, 2, "HS") == 0) {
        r.error = "asymmetric verify rejects alg=" + alg; // 防算法混淆攻击
        return r;
    }
    const EVP_MD* md = mdFor(alg);
    if (md == nullptr) {
        r.error = "unsupported alg: " + alg;
        return r;
    }

    PkeyPtr key = readPublic(publicPem);
    if (key == nullptr) {
        r.error = "invalid public key PEM";
        return r;
    }

    auto sigRaw = detail::unb64url(token.substr(d2 + 1));
    if (!sigRaw.has_value()) {
        r.error = "bad signature encoding";
        return r;
    }

    const size_t fieldLen = fieldLenFor(alg);
    std::vector<uint8_t> der;
    const uint8_t* sigPtr = reinterpret_cast<const uint8_t*>(sigRaw->data());
    size_t sigLen = sigRaw->size();
    if (fieldLen != 0) {
        if (sigLen != fieldLen * 2) {
            r.error = "bad ECDSA signature length";
            return r;
        }
        der = rawToDer(sigPtr, fieldLen);
        sigPtr = der.data();
        sigLen = der.size();
    }

    const std::string signingInput = token.substr(0, d2);
    MdCtx ctx;
    if (!ctx.ok()) {
        r.error = "EVP_MD_CTX_new failed";
        return r;
    }
    if (EVP_DigestVerifyInit(ctx.p, nullptr, md, nullptr, key.get()) != 1) {
        r.error = "verify init failed";
        return r;
    }
    if (EVP_DigestVerifyUpdate(ctx.p, signingInput.data(), signingInput.size()) != 1) {
        r.error = "verify update failed";
        return r;
    }
    if (EVP_DigestVerifyFinal(ctx.p, sigPtr, sigLen) != 1) {
        r.error = "bad signature";
        return r;
    }

    auto payload = detail::unb64url(token.substr(d1 + 1, d2 - d1 - 1));
    if (!payload.has_value()) {
        r.error = "bad payload encoding";
        return r;
    }
    const auto exp = claimInt(*payload, "exp");
    if (exp.has_value() && nowSec > *exp + leewaySec) {
        r.error = "token expired";
        return r;
    }
    r.ok = true;
    r.payload = std::move(*payload);
    return r;
}

} // namespace zenith::http::jwt::asym

#endif // ZENITH_HAS_OPENSSL
