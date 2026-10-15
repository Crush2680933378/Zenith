#!/usr/bin/env python3
# ============================================================================
#  zenith-new —— Zenith 工程脚手架（M5.5）
#  用法：python zenith_new.py my_app [目标目录]
#  生成：CMakeLists.txt / main.cpp / zenith.json / .gitignore / README.md
# ============================================================================
import os
import sys

CMAKE = '''cmake_minimum_required(VERSION 3.16)
project({name} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Zenith：改为你本机的 Zenith 源码路径或安装路径（find_package 安装形态）
set(ZENITH_ROOT "$ENV{{ZENITH_ROOT}}" CACHE PATH "Zenith root")
if(NOT ZENITH_ROOT)
    message(FATAL_ERROR "请设置 ZENITH_ROOT 指向 Zenith 仓库/安装目录")
endif()

add_executable({name} main.cpp)
target_include_directories({name} PRIVATE ${{ZENITH_ROOT}}/include)
target_compile_definitions({name} PRIVATE ZENITH_STATIC)  # 静态库消费（M1 规则）
# 静态库形态：显式链 libzenith.a 全路径（MinGW 下 -lzenith 会优先命中
# 同目录导入库 libzenith.dll.a，其只含动态导出符号集）
if(WIN32)
    target_link_libraries({name} PRIVATE ${{ZENITH_ROOT}}/lib/libzenith.a ws2_32)
else()
    target_link_libraries({name} PRIVATE ${{ZENITH_ROOT}}/lib/libzenith.a pthread)
endif()
'''

MAIN = '''#include <thread>
#include "zenith/concurrency/thread_pool.hpp"
#include "zenith/http/http_client.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/http/security.hpp"
#include "zenith/http/session.hpp"
#include "zenith/log/console_logger.hpp"
#include "zenith/log/logger.hpp"
#include "zenith/net/signal.hpp"

int main() {{
    using namespace zenith;
    auto& facade = log::LoggerFacade::instance();
    facade.addSink(std::make_shared<log::ConsoleLogger>(true, true));

    net::EventLoop loop;
    net::EventLoopThreadPool io(4);
    io.start();
    http::HttpServer server(&loop, &io);

    auto sessions = std::make_shared<http::SessionManager>(
        std::make_shared<http::MemorySessionStore>());
    server.router().use(http::requestIdMiddleware());
    server.router().use(http::metricsMiddleware());
    server.router().use(http::sessionMiddleware(sessions));
    server.router().use(http::securityHeadersMiddleware());

    server.router().get("/hello", [](http::Context& c) {{
        c.text(200, "Hello from {name}!");
    }});
    server.router().get("/api/users/:id", [](http::Context& c) {{
        http::json::JsonValue v = http::json::JsonValue::object();
        v["id"] = c.param("id");
        v["visitor"] = c.session()->get("count", "first");
        c.json(200, v);
    }});
    server.router().get("/metrics", http::prometheusHandler());
    server.router().get("/zenith/status", http::statusHandler());

    if (!server.listen("0.0.0.0", 8080)) return 1;
    ZLOG_INFO("{name} listening on http://127.0.0.1:8080");

    net::installSignalHandler([&] {{
        ZLOG_INFO("shutting down...");
        server.beginGracefulShutdown([&] {{ loop.stop(); }});
    }});
    loop.loop();
    io.stop();
    facade.shutdown();
    return 0;
}}
'''

CONFIG = '''# zenith 应用配置（KeyValueConfig/JsonConfig 兼容）
server.host = 0.0.0.0
server.port = 8080
log.level = info
'''

GITIGNORE = '''build/
*.log
.vscode/
.idea/
'''

README = '''# {name}

Zenith 脚手架工程。构建：

```bash
# 1) 先构建 Zenith（<zenith-repo>）
cmake --preset win-mingw-debug && cmake --build --preset win-mingw-debug

# 2) 本工程
export ZENITH_ROOT=/path/to/zenith      # 或 Windows set ZENITH_ROOT=...
cmake -B build && cmake --build build
./build/{name}
```

路由示例见 main.cpp；更多见 Zenith 仓库 examples/。
'''


def main():
    if len(sys.argv) < 2:
        print("用法: python zenith_new.py <app-name> [目标目录]")
        sys.exit(1)
    name = sys.argv[1]
    target = sys.argv[2] if len(sys.argv) > 2 else name
    os.makedirs(target, exist_ok=True)
    files = {
        "CMakeLists.txt": CMAKE.format(name=name),
        "main.cpp": MAIN.format(name=name),
        "zenith.json": CONFIG,
        ".gitignore": GITIGNORE,
        "README.md": README.format(name=name),
    }
    for fn, content in files.items():
        p = os.path.join(target, fn)
        with open(p, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)
        print(f"  + {p}")
    print(f"\n脚手架完成：{target}/（设置 ZENITH_ROOT 后 cmake -B build）")


if __name__ == "__main__":
    main()
