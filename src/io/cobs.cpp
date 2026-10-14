#include "zenith/io/cobs.hpp"

namespace zenith::io {

uint32_t crc32(const uint8_t* data, size_t n, uint32_t seed) {
    // 逐位反射实现（表驱动留作后续优化；正确性优先）
    uint32_t crc = ~seed;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) {
            const uint32_t mask = (crc & 1u) ? 0xEDB88320u : 0u;
            crc = (crc >> 1) ^ mask;
        }
    }
    return ~crc;
}

std::string cobsEncode(const uint8_t* data, size_t n) {
    // 输出最长 n + ceil(n/254) + 1
    std::string out;
    out.reserve(n + n / 254 + 2);
    size_t blockStart = 0; // 当前块起点在 out 中的占位符位置
    out.push_back('\x01'); // 第一个距离码占位
    size_t run = 0;        // 距离计数（自上一 0x00 起）

    for (size_t i = 0; i < n; ++i) {
        if (data[i] == 0x00) {
            out[blockStart] = static_cast<char>(run + 1); // 距离码
            blockStart = out.size();
            out.push_back('\x01'); // 新块占位
            run = 0;
        } else {
            out.push_back(static_cast<char>(data[i]));
            ++run;
            if (run == 254) { // 满块：码 255 = 254 字面量且不隐含 0
                out[blockStart] = static_cast<char>(255);
                blockStart = out.size();
                out.push_back('\x01');
                run = 0;
            }
        }
    }
    out[blockStart] = static_cast<char>(run + 1); // 尾块距离码
    return out;
}

std::optional<std::string> cobsDecode(const uint8_t* data, size_t n) {
    if (n == 0) return std::nullopt;
    std::string out;
    out.reserve(n);
    size_t i = 0;
    while (i < n) {
        const uint8_t code = data[i++];
        if (code == 0) return std::nullopt; // 0x00 不应出现在帧内
        const size_t take = static_cast<size_t>(code - 1);
        if (i + take > n) return std::nullopt; // 截断
        out.append(reinterpret_cast<const char*>(data + i), take);
        i += take;
        if (i < n && code != 255) {
            // 非最大距离码后隐含一个 0x00
            out.push_back('\0');
        }
    }
    return out;
}

std::vector<std::string> frame::Deframer::feed(const uint8_t* data, size_t n) {
    std::vector<std::string> frames;
    for (size_t i = 0; i < n && healthy_; ++i) {
        if (data[i] == 0x00) {
            // 帧结束
            if (!buf_.empty()) {
                auto payload = frame::decode(reinterpret_cast<const uint8_t*>(buf_.data()),
                                             buf_.size());
                if (payload.has_value()) frames.push_back(std::move(payload).value());
                // CRC 失败 → 丢弃该帧（链路误码），保持流同步
                buf_.clear();
            }
            continue;
        }
        buf_.push_back(static_cast<char>(data[i]));
        if (buf_.size() > kMaxFrameBytes) {
            healthy_ = false;
            buf_.clear();
        }
    }
    return frames;
}

void frame::Deframer::reset() {
    buf_.clear();
    healthy_ = true;
}

} // namespace zenith::io
