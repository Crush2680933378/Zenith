// ============================================================================
//  Buffer —— 网络字节缓冲区（M2 net/http 层的地基，纯头文件实现）
//  布局：[ readerIndex_ ... writerIndex_ ... capacity )
//    - 可读区间：[readerIndex_, writerIndex_)
//    - consume() 前移读指针；空间不足时自动将未读数据挪回首部
// ============================================================================
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace zenith::core {

class Buffer {
public:
    static constexpr size_t kInitialCapacity = 1024;

    explicit Buffer(size_t initialCapacity = kInitialCapacity) : buf_(initialCapacity) {}

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

    /// M7.2c：空闲缩容——容量远超阈值且几乎无堆积时收缩，防瞬时大响应把
    /// 连接缓冲永久撑大。writeComplete（发送排空）后调用，此时无在途写指针。
    ///
    /// 注意顺序：必须**先紧凑化再截断**。反过来的话 resize 会先把
    /// [readerIndex_, writerIndex_) 里的未读数据截掉，随后 memmove 搬的是
    /// 已失效区间（sanitizer/debug STL 下直接暴露）。
    void shrinkIfBloated(size_t threshold) {
        if (buf_.size() <= threshold || readableBytes() >= threshold / 4) return;
        compact();
        const size_t need = kPrependBytes + readableBytes();
        if (buf_.size() - need <= threshold / 2) {
            buf_.resize(need);  // 冗余不多：仅退回逻辑长度，保留已分配内存
            return;
        }
        // 冗余明显（如一次性 8MB 下载排空后）：真正换小缓冲把内存还给分配器
        std::vector<uint8_t> fresh(std::max(need, kInitialCapacity));
        if (need > kPrependBytes) std::memcpy(fresh.data(), buf_.data(), need);
        buf_.swap(fresh);
    }

    void ensureWritable(size_t n) {
        if (writableBytes() >= n) return;
        // 需求容量 = 前部保留 + 未读数据 + 新数据；不足则扩容，随后紧凑化
        const size_t required = kPrependBytes + readableBytes() + n;
        if (buf_.size() < required) {
            buf_.resize(required);
        }
        compact();
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

    uint8_t readUint8() { return readIntegralAs<uint8_t>(); }
    uint16_t readUint16LE() { return readIntegral<uint16_t, false>(); }
    uint32_t readUint32LE() { return readIntegral<uint32_t, false>(); }
    uint64_t readUint64LE() { return readIntegral<uint64_t, false>(); }
    uint16_t readUint16BE() { return readIntegral<uint16_t, true>(); }
    uint32_t readUint32BE() { return readIntegral<uint32_t, true>(); }
    uint64_t readUint64BE() { return readIntegral<uint64_t, true>(); }

private:
    static constexpr size_t kPrependBytes = 8;  // 预留前部空间（协议头注入用）

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

}  // namespace zenith::core
