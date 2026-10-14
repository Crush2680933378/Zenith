// ============================================================================
//  Buffer —— 网络字节缓冲区（M2 net/http 层的地基，纯头文件实现）
//  布局：[ readerIndex_ ... writerIndex_ ... capacity )
//    - 可读区间：[readerIndex_, writerIndex_)
//    - consume() 前移读指针；空间不足时自动将未读数据挪回首部
// ============================================================================
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace zenith::core {

class Buffer {
public:
    static constexpr size_t kInitialCapacity = 1024;

    explicit Buffer(size_t initialCapacity = kInitialCapacity)
        : buf_(initialCapacity) {}

    /* ---- 写入 ---- */
    void append(const void* data, size_t n) {
        ensureWritable(n);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        std::memcpy(buf_.data() + writerIndex_, p, n);
        writerIndex_ += n;
    }

    void appendString(const std::string& s) { append(s.data(), s.size()); }

    /* ---- 容量/状态 ---- */
    [[nodiscard]] size_t readableBytes() const noexcept { return writerIndex_ - readerIndex_; }
    [[nodiscard]] size_t writableBytes() const noexcept { return buf_.size() - writerIndex_; }
    [[nodiscard]] size_t capacity() const noexcept { return buf_.size(); }

    void ensureWritable(size_t n) {
        if (writableBytes() >= n) return;
        if (buf_.size() - readableBytes() + writableBytes() >= n + kPrependBytes
            && readableBytes() < kCompactThreshold) {
            // 剩余空间足够：把未读数据挪到 kPrependBytes 处，避免扩容
            compact();
        } else {
            buf_.resize(writerIndex_ + n);
        }
    }

    /* ---- 读取（消费式） ---- */
    [[nodiscard]] const uint8_t* peek() const noexcept { return buf_.data() + readerIndex_; }

    void consume(size_t n) {
        if (n > readableBytes()) n = readableBytes();
        readerIndex_ += n;
        if (readerIndex_ == writerIndex_) retrieveAll();
    }

    void retrieveAll() noexcept { readerIndex_ = writerIndex_ = kPrependBytes; }

    /// 读取 n 字节并消费；不足 n 抛出异常。
    std::string readString(size_t n) {
        if (n > readableBytes()) throw std::out_of_range("Buffer::readString: not enough bytes");
        std::string s(reinterpret_cast<const char*>(peek()), n);
        consume(n);
        return s;
    }

    std::string readAllAsString() { return readString(readableBytes()); }

    uint8_t  readUint8()  { return readIntegralAs<uint8_t>(); }
    uint16_t readUint16LE() { return readIntegral<uint16_t, false>(); }
    uint32_t readUint32LE() { return readIntegral<uint32_t, false>(); }
    uint64_t readUint64LE() { return readIntegral<uint64_t, false>(); }
    uint16_t readUint16BE() { return readIntegral<uint16_t, true>(); }
    uint32_t readUint32BE() { return readIntegral<uint32_t, true>(); }
    uint64_t readUint64BE() { return readIntegral<uint64_t, true>(); }

private:
    static constexpr size_t kPrependBytes      = 8;     // 预留前部空间（协议头注入用）
    static constexpr size_t kCompactThreshold  = 4096;  // 未读数据小于该值才做紧凑挪动

    void compact() {
        const size_t n = readableBytes();
        std::memmove(buf_.data() + kPrependBytes, buf_.data() + readerIndex_, n);
        readerIndex_ = kPrependBytes;
        writerIndex_ = readerIndex_ + n;
    }

    template <typename U>
    U readIntegralAs() {
        if (readableBytes() < sizeof(U)) throw std::out_of_range("Buffer: underflow");
        U v;
        std::memcpy(&v, peek(), sizeof(U));
        consume(sizeof(U));
        return v;
    }

    /// BigEndian=true 按网络序读取，否则按小端。
    template <typename U, bool BigEndian>
    U readIntegral() {
        U v = readIntegralAs<U>();
        if constexpr (BigEndian) {
            U swapped = 0;
            for (size_t i = 0; i < sizeof(U); ++i) {
                swapped = static_cast<U>((swapped << 8) | ((v >> (i * 8)) & 0xFF));
            }
            return swapped;
        } else {
            return v;
        }
    }

    std::vector<uint8_t> buf_;
    size_t readerIndex_ = kPrependBytes;
    size_t writerIndex_ = kPrependBytes;
};

} // namespace zenith::core
