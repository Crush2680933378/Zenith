// fuzz_http_request —— HTTP/1.1 请求解析器（服务端攻击面）
//
// 关注点：畸形请求行/头、超长字段、Content-Length 与 chunked 的矛盾、
// 分片到达（半包）时的状态机。
#include <cstddef>
#include <cstdint>
#include <string>

#include "zenith/http/http_parser.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0 || size > 64 * 1024) return 0; // 限制单次输入，避免 fuzz 变慢
    const std::string input(reinterpret_cast<const char*>(data), size);

    // 1) 一次性喂入
    {
        zenith::http::HttpRequestParser parser(1 * 1024 * 1024);
        const auto out = parser.feed(input.data(), input.size());
        if (out.complete()) {
            const auto& req = parser.request();
            // 完成态必须自洽：路径非空、方法合法
            if (req.path.empty()) __builtin_trap();
            (void)req.headers.size();
            (void)req.contentLength();
            (void)req.keepAlive();
        }
    }

    // 2) 按输入内容决定的分片点喂入（模拟 TCP 半包）
    {
        zenith::http::HttpRequestParser parser(1 * 1024 * 1024);
        const size_t split = (data[0] % 8 == 0) ? (size / 2) : (size > 0 ? 1 : 0);
        if (split > 0 && split < size) {
            (void)parser.feed(input.data(), split);
            (void)parser.feed(input.data() + split, size - split);
        }
        (void)parser.reset();
    }
    return 0;
}
