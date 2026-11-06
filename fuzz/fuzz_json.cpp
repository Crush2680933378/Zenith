// fuzz_json —— 自研 JSON 解析器（配置/请求体/上游响应都走它）
#include <cstddef>
#include <cstdint>
#include <string>

#include "zenith/json/json.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size > 64 * 1024) return 0;
    const std::string input(reinterpret_cast<const char*>(data), size);

    auto v = zenith::json::JsonValue::parse(input);
    if (v) {
        // 解析成功 → dump 必须仍是合法 JSON（能被再次解析）
        const std::string dumped = v.value().dump();
        auto again = zenith::json::JsonValue::parse(dumped);
        if (!again) __builtin_trap();
    }
    return 0;
}
