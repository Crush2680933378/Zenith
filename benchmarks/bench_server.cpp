// ============================================================================
//  基准服务端（仅本地/CI 性能对比用，不参与默认构建产物）
//
//  用途：给 tools/zenith_bench 提供两个端点，量化 M7.3 零拷贝与 M7.2 内存优化：
//    GET /big.bin   4MB 二进制（image/png → 走零拷贝路径）
//    GET /text.txt  4MB 文本   （text/plain → 客户端带 gzip 时走缓冲路径）
//    GET /json      小 JSON   （回归基线：热路径不能变慢）
//
//  用法：bench_server <port> <seconds>
// ============================================================================
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "zenith/http/http_server.hpp"
#include "zenith/http/static_files.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    const int port = argc > 1 ? std::atoi(argv[1]) : 8099;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;
    const size_t kSize = argc > 3 ? static_cast<size_t>(std::atoi(argv[3])) * 1024u * 1024u
                                  : 4u * 1024u * 1024u;
    // 第 4 个参数：数据目录（绝对路径最稳；默认 ./benchwww）
    const std::string root = argc > 4 ? argv[4] : "benchwww";

    std::error_code ec;
    fs::create_directories(root, ec);
    if (ec) std::printf("  data dir create failed: %s\n", ec.message().c_str());
    for (const char* name : {"big.bin", "text.txt"}) {
        const fs::path p = fs::path(root) / name;
        if (fs::exists(p, ec) && fs::file_size(p, ec) == kSize) continue;
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        std::string chunk(kSize, '\0');
        for (size_t i = 0; i < kSize; ++i) chunk[i] = static_cast<char>((i * 31u + 7u) & 0xFFu);
        if (std::string(name) == "text.txt") chunk.assign(kSize, 'T');
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (!out.good()) std::printf("  data write failed: %s\n", p.string().c_str());
    }

    zenith::net::EventLoop loop;
    zenith::net::EventLoopThreadPool io{4};
    io.start();

    zenith::http::HttpServer server(&loop, &io);
    server.router().get("/big.bin", zenith::http::staticHandler(root, ""));
    server.router().get("/text.txt", zenith::http::staticHandler(root, ""));
    server.router().get("/json", [](zenith::http::Context& c) {
        zenith::http::json::JsonValue v = zenith::http::json::JsonValue::object();
        v["ok"] = true;
        v["n"] = static_cast<int64_t>(42);
        c.json(200, v);
    });

    if (!server.listen("127.0.0.1", static_cast<uint16_t>(port))) {
        std::fprintf(stderr, "listen failed on %d\n", port);
        return 1;
    }
    std::printf("bench_server listening on 127.0.0.1:%d (exit in %ds)\n", port, seconds);
    std::fflush(stdout);

    std::thread timer([&loop, seconds] {
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        loop.stop();
    });
    loop.loop();
    timer.join();
    io.stop();
    return 0;
}
