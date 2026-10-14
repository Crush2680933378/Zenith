// ============================================================================
//  zenith::crypto::sha1 —— 自研 SHA-1（RFC 3174）
//  仅用于 WebSocket 握手 Sec-WebSocket-Accept 等非安全敏感场景；
//  密码学安全用途请等待 M4 TLS 层（走 OpenSSL/mbedTLS）。
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <string>

namespace zenith::crypto {

class Sha1 {
public:
    Sha1() { reset(); }

    void reset() {
        h_[0] = 0x67452301u;
        h_[1] = 0xEFCDAB89u;
        h_[2] = 0x98BADCFEu;
        h_[3] = 0x10325476u;
        h_[4] = 0xC3D2E1F0u;
        len_ = 0;
        bufLen_ = 0;
    }

    void update(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        len_ += n;
        if (bufLen_ > 0) {
            const size_t take = (n < 64 - bufLen_) ? n : (64 - bufLen_);
            std::memcpy(buf_ + bufLen_, p, take);
            bufLen_ += take;
            p += take;
            n -= take;
            if (bufLen_ == 64) {
                block(buf_);
                bufLen_ = 0;
            }
        }
        while (n >= 64) {
            block(p);
            p += 64;
            n -= 64;
        }
        if (n > 0) {
            std::memcpy(buf_, p, n);
            bufLen_ = n;
        }
    }

    void update(const std::string& s) { update(s.data(), s.size()); }

    /// 完成并输出 20 字节摘要（之后需 reset 才能复用）。
    void final(uint8_t out[20]) {
        const uint64_t bitLen = len_ * 8;
        uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0x00;
        while (bufLen_ != 56) update(&pad, 1);
        uint8_t lenBytes[8];
        for (int i = 0; i < 8; ++i) lenBytes[i] = static_cast<uint8_t>(bitLen >> (56 - 8 * i));
        update(lenBytes, 8);

        for (int i = 0; i < 5; ++i) {
            out[i * 4 + 0] = static_cast<uint8_t>(h_[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h_[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h_[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h_[i]);
        }
    }

    /// 便捷：一次性计算十六进制摘要（小写）。
    static std::string hex(const std::string& data) {
        Sha1 s;
        s.update(data);
        uint8_t d[20];
        s.final(d);
        static const char* kHex = "0123456789abcdef";
        std::string out;
        out.reserve(40);
        for (uint8_t b : d) {
            out.push_back(kHex[b >> 4]);
            out.push_back(kHex[b & 0xF]);
        }
        return out;
    }

private:
    static uint32_t rol(uint32_t v, int s) noexcept {
        return (v << s) | (v >> (32 - s));
    }

    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (static_cast<uint32_t>(p[i * 4]) << 24) |
                   (static_cast<uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<uint32_t>(p[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }
        uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h_[0] += a;
        h_[1] += b;
        h_[2] += c;
        h_[3] += d;
        h_[4] += e;
    }

    uint32_t h_[5];
    uint64_t len_ = 0;
    uint8_t buf_[64];
    size_t bufLen_ = 0;
};

} // namespace zenith::crypto
