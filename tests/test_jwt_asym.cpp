// ============================================================================
//  M9.2 非对称 JWT 测试：RS256 / ES256 签发校验 + 算法混淆防护
//  交叉核对：ECDSA 签名必须是 64 字节 R||S（JWS 定长），RS 签名可被 OpenSSL
//  自身验证通过——两侧都走 EVP，任一侧 DER/定长拼接有偏差都会失败。
// ============================================================================
#include <string>

#include "ztest.hpp"

#include "zenith/http/jwt.hpp"

using namespace zenith;

namespace {
constexpr int64_t kNow = 1790000000; // 固定时间基准，避免时钟敏感
const std::string kPayload = R"({"sub":"u-1","scope":"admin"})";
} // namespace

ZTEST(JwtAsym, Rsa256RoundTrip) {
    auto kp = http::jwt::asym::generateRsa(2048);
    ZEXPECT_TRUE(kp.isOk());
    if (kp.isErr()) return;

    auto token = http::jwt::asym::sign(kp.value(), kPayload, /*expiresIn=*/60, kNow);
    ZEXPECT_TRUE(token.isOk());
    if (token.isErr()) return;

    ZEXPECT_EQ(http::jwt::headerAlg(token.value()), std::string("RS256"));

    auto r = http::jwt::asym::verify(kp.value().publicPem, token.value(), kNow);
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_TRUE(r.payload.find("\"sub\":\"u-1\"") != std::string::npos);

    // 过期（签发时间早于校验时间 120s，exp=60）
    auto expired = http::jwt::asym::verify(kp.value().publicPem, token.value(), kNow + 120, 0);
    ZEXPECT_FALSE(expired.ok);
    ZEXPECT_TRUE(expired.error.find("expired") != std::string::npos);
}

ZTEST(JwtAsym, RsaTamperedPayloadRejected) {
    auto kp = http::jwt::asym::generateRsa(2048);
    ZEXPECT_TRUE(kp.isOk());
    if (kp.isErr()) return;
    auto token = http::jwt::asym::sign(kp.value(), kPayload, 60, kNow);
    ZEXPECT_TRUE(token.isOk());
    if (token.isErr()) return;

    // 改一位 payload 后签名必须失效
    std::string bad = token.value();
    const size_t d1 = bad.find('.');
    const size_t firstPayloadChar = d1 + 1;
    bad[firstPayloadChar] = bad[firstPayloadChar] == 'e' ? 'f' : 'e';
    auto r = http::jwt::asym::verify(kp.value().publicPem, bad, kNow);
    ZEXPECT_FALSE(r.ok);
}

ZTEST(JwtAsym, Ec256SignatureIsRawNotDer) {
    auto kp = http::jwt::asym::generateEcP256();
    ZEXPECT_TRUE(kp.isOk());
    if (kp.isErr()) return;

    auto token = http::jwt::asym::sign(kp.value(), kPayload, 60, kNow);
    ZEXPECT_TRUE(token.isOk());
    if (token.isErr()) return;
    ZEXPECT_EQ(http::jwt::headerAlg(token.value()), std::string("ES256"));

    // JWS 的 ES256 签名是 R||S 共 64 字节，base64url 后应为 86 字符（无填充）
    const std::string sig = token.value().substr(token.value().rfind('.') + 1);
    ZEXPECT_EQ(sig.size(), static_cast<size_t>(86));

    auto r = http::jwt::asym::verify(kp.value().publicPem, token.value(), kNow);
    ZEXPECT_TRUE(r.ok);
    ZEXPECT_TRUE(r.payload.find("admin") != std::string::npos);
}

ZTEST(JwtAsym, RejectsNoneAndHmacAlgorithms) {
    auto kp = http::jwt::asym::generateRsa(2048);
    ZEXPECT_TRUE(kp.isOk());
    if (kp.isErr()) return;

    // alg=none 的裸 token：header.payload.，必须被拒
    const std::string noneToken = http::jwt::detail::b64url("{\"alg\":\"none\",\"typ\":\"JWT\"}") +
                                  "." + http::jwt::detail::b64url(kPayload) + ".";
    auto rNone = http::jwt::asym::verify(kp.value().publicPem, noneToken, kNow);
    ZEXPECT_FALSE(rNone.ok);
    ZEXPECT_TRUE(rNone.error.find("none") != std::string::npos);

    // HS256 token 不能拿公钥验（算法混淆攻击面）
    const std::string hs = http::jwt::sign("secret", kPayload, 60, kNow);
    auto rHs = http::jwt::asym::verify(kp.value().publicPem, hs, kNow);
    ZEXPECT_FALSE(rHs.ok);
    ZEXPECT_EQ(http::jwt::headerAlg(hs), std::string("HS256"));
}

ZTEST(JwtAsym, WrongPublicKeyRejected) {
    auto a = http::jwt::asym::generateEcP256();
    auto b = http::jwt::asym::generateEcP256();
    ZEXPECT_TRUE(a.isOk() && b.isOk());
    if (a.isErr() || b.isErr()) return;

    auto token = http::jwt::asym::sign(a.value(), kPayload, 60, kNow);
    ZEXPECT_TRUE(token.isOk());
    if (token.isErr()) return;

    auto r = http::jwt::asym::verify(b.value().publicPem, token.value(), kNow);
    ZEXPECT_FALSE(r.ok); // 另一把公钥验不过
}

ZTEST_MAIN
