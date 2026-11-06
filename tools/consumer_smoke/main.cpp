// 下游消费者冒烟：只通过 find_package(Zenith CONFIG) 使用已安装的 SDK，
// 不引用源仓库任何路径。用来验证「安装导出」这条对外链路真的能编译+链接+运行。
#include <cstdio>
#include <string>

#include "zenith/core/buffer.hpp"
#include "zenith/json/json.hpp"

int main() {
    zenith::core::Buffer buf;
    buf.appendString("{\"ok\":true}");
    const std::string raw = buf.readAllAsString();

    auto v = zenith::json::JsonValue::parse(raw);
    if (!v) {
        std::printf("consumer: json parse failed: %s\n", v.error().message.c_str());
        return 1;
    }
    std::printf("consumer ok: %s\n", v.value().dump().c_str());
    return 0;
}
