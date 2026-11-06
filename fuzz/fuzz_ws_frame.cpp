// fuzz_ws_frame —— WebSocket 帧解码（RFC6455：长度扩展/掩码/分片）
#include <cstddef>
#include <cstdint>

#include "zenith/net/websocket.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 64 * 1024) return 0;

    // 服务端视角（要求客户端帧带掩码）与客户端视角（不带掩码）都要过一遍
    const auto serverSide = zenith::net::wsDecodeFrame(data, size, /*requireMask=*/true);
    if (serverSide.state == zenith::net::WsDecodeResult::State::Ok) {
        // 解出的载荷长度不能超过输入；opcode 必须是合法枚举
        if (serverSide.frame.payload.size() > size) __builtin_trap();
        const auto op = static_cast<uint8_t>(serverSide.frame.opcode);
        if (op > 0x0A) __builtin_trap();
    }

    (void)zenith::net::wsDecodeFrame(data, size, /*requireMask=*/false);
    return 0;
}
