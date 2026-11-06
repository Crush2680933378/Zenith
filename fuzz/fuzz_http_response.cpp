// fuzz_http_response —— HTTP/1.1 响应解析器（客户端攻击面：恶意上游）
#include <cstddef>
#include <cstdint>
#include <string>

#include "zenith/http/http_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 64 * 1024) return 0;
    const std::string input(reinterpret_cast<const char*>(data), size);

    zenith::http::HttpResponseParser parser;
    const auto out = parser.feed(input.data(), input.size());
    if (out.complete()) {
        const auto& resp = parser.response();
        // 状态码必须在合法区间，否则上层会按错误码误判
        if (resp.status < 100 || resp.status > 599) __builtin_trap();
        (void)resp.keepAlive();
        (void)resp.body.size();
    }
    return 0;
}
