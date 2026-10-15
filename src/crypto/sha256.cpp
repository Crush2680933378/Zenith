#include "zenith/crypto/sha256.hpp"

namespace zenith::crypto {

void hmacSha256(const void* key, size_t keyLen, const void* msg, size_t msgLen,
                uint8_t out[32]) {
    uint8_t k[64] = {0};
    if (keyLen > 64) {
        Sha256 h;
        h.update(key, keyLen);
        h.final(k); // 长密钥 → 先散列
    } else {
        std::memcpy(k, key, keyLen);
    }
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = static_cast<uint8_t>(k[i] ^ 0x36);
        opad[i] = static_cast<uint8_t>(k[i] ^ 0x5c);
    }
    uint8_t inner[32];
    Sha256 h1;
    h1.update(ipad, 64);
    h1.update(msg, msgLen);
    h1.final(inner);

    Sha256 h2;
    h2.update(opad, 64);
    h2.update(inner, 32);
    h2.final(out);
}

bool constantTimeEquals(const uint8_t* a, const uint8_t* b, size_t n) noexcept {
    uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
    return diff == 0;
}

} // namespace zenith::crypto
