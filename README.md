# Zenith

[![CI](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml/badge.svg)](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/Crush2680933378/Zenith?include_prereleases)](https://github.com/Crush2680933378/Zenith/releases)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

**天顶，顶点，万物之巅。** Zenith 是一款 C++17 高并发后端框架，定位对标 Spring Boot / Gin，
以**纯 C ABI 插件内核**为地基：TLS、媒体、LLM 推理、多 Agent 协作、向量库、语音——
全部通过稳定 ABI 或可选编译模块接入，宿主不为任何三方库承担 C++ ABI 兼容义务。

前端交给 React，Zenith 只提供 API。

> **237K RPS / 9.2 MB 内存**（裸 JSON，10 框架横评第一）· **43 套测试** · **85.8% 行覆盖** ·
> ASan / TSan / fuzz / 性能门禁全在 CI 里

---

## 特性

| 领域 | 能力 |
|---|---|
| **网络** | Reactor（WSAPoll / epoll / kqueue / select 四后端）、TcpServer/TcpConnection/TcpClient、TLS（OpenSSL，内存 BIO 握手） |
| **HTTP** | 自研 HTTP/1.1 解析器、路由树（`:param` / `*catchAll`）、洋葱中间件、SSE/chunked 流式、gzip、静态文件（Range/ETag/**零拷贝**）、multipart、Session、CORS、限流 |
| **协议** | WebSocket（RFC 6455 全实现）、Redis RESP2 客户端、MQTT 3.1.1 客户端、MCP（JSON-RPC） |
| **数据** | uORM 服务化（连接池 + 专用线程池，不占 EventLoop）、自研 HNSW 向量库（Qdrant 兼容 API） |
| **AI 后端** | LLM 推理网关（OpenAI 兼容面）、三协议厂商路由（openai / anthropic / gemini）+ 熔断成本、ReAct Agent + RAG、7 角色 SDLC-Crew、输入护栏（注入/PII）、whisper STT |
| **工程** | 纯 C 插件 ABI + 热重载、Prometheus 指标、`/zenith/status`、`find_package(Zenith)` 导出、协程层（可选 C++20） |

自研优先的边界：HTTP/1.1、WebSocket、JSON、EventLoop/Poller、路由、日志、线程池、
Redis RESP、JWT、SHA/HMAC、Base64 全部自己写——详见 [依赖原则](docs/DEPENDENCIES.md)。

## 快速开始

```bash
git clone https://github.com/Crush2680933378/Zenith && cd Zenith
git submodule update --init --recursive          # uORM + uJSON（可选：-DZENITH_ENABLE_ORM=OFF）

# 用 CMakePresets（推荐）
cmake --preset linux-gcc-debug && cmake --build --preset linux-gcc-debug
ctest --preset linux-gcc-debug

# 或者手写
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DZENITH_TLS=openssl -DZENITH_ENABLE_ORM=OFF
cmake --build build && ctest --test-dir build --output-on-failure

# 跑示例（插件宿主 / HTTP 服务）
./build/bin/plugin_host
```

平台差异：

- **Windows**：MSYS2 MinGW-w64，`cmake --preset win-mingw-debug`
- **Linux**：GCC 或 Clang（协程层用 `linux-clang-debug` + `ZENITH_ENABLE_COROUTINES=ON`）
- **交叉编译**：`cmake/toolchains/aarch64-linux-gnu.cmake`（泰山派 RK3566/RK3576）

### 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `ZENITH_BUILD_STATIC` / `ZENITH_BUILD_SHARED` | ON / ON | `zenith_core`（静态）/ `zenith`（动态） |
| `ZENITH_BUILD_TESTS` / `EXAMPLES` / `PLUGINS` | ON | 测试 / 示例 / 示例插件 |
| `ZENITH_TLS` | none | `openssl`（其余后端留位） |
| `ZENITH_ENABLE_ORM` | ON | 集成 uORM 子模块 |
| `ZENITH_ENABLE_COROUTINES` | OFF | C++20 协程层（不影响核心 C++17） |
| `ZENITH_SANITIZE` / `ZENITH_COVERAGE` / `ZENITH_BUILD_FUZZERS` | 关 | CI 用的内存/覆盖率/fuzz 构建 |

## 用 20 行起一个服务

```cpp
#include "zenith/http/compress.hpp"
#include "zenith/http/http_server.hpp"
#include "zenith/http/observability.hpp"
#include "zenith/net/event_loop.hpp"
#include "zenith/net/event_loop_thread_pool.hpp"

int main() {
    zenith::net::EventLoop loop;
    zenith::net::EventLoopThreadPool io{4};
    io.start();

    zenith::http::HttpServer server(&loop, &io);
    auto& r = server.router();
    r.use(zenith::http::requestIdMiddleware());
    r.use(zenith::http::gzipMiddleware());
    r.get("/hello", [](zenith::http::Context& c) { c.text(200, "Hello, Zenith!"); });
    r.get("/users/:id", [](zenith::http::Context& c) {
        auto v = zenith::http::json::JsonValue::object();
        v["id"] = c.param("id");
        c.json(200, v);
    });

    if (!server.listen("0.0.0.0", 8080)) return 1;
    loop.loop();
}
```

SSE 流式、静态文件、WebSocket、TLS 各是一行（`c.sse()` / `staticHandler()` /
`enableWebSocket()` / `listenTls()`），示例见 `examples/`。

## 插件体系

宿主只认 `zenith/abi/zenith_abi.h` 里的 C 结构体——**纯 C 编译器就能写插件**：

```c
static zenith_status my_init(const zenith_host_api* host) {
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  "my.IService", 1, &my_instance, NULL);
}
static zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc), ZENITH_ABI_VERSION, "my-plugin", "0.2.2",
    "desc", "you", &my_init, NULL, NULL, NULL, {NULL, NULL, NULL, NULL},
};
ZENITH_EXPORT const zenith_plugin_desc* zenith_plugin_entry(void) { return &g_desc; }
```

宿主侧：`PluginManager::loadDirectory()` → `startAll()` → `registry.queryRaw("my.IService", 1)`。

仓库内 16 个插件（2 个示例 + 14 个生产插件：推理网关 / 三协议路由 / 多 Agent 集群 /
向量库 / MCP / 护栏 / 语音 / FFmpeg 媒体 / ESP32 设备桥…），
完整清单与热重载细节见 **[docs/PLUGINS.md](docs/PLUGINS.md)**。

## 性能

### 跨语言公平基准（10 框架）

同机 loopback / 统一客户端 `zenith-bench`（2 线程 × 8 keep-alive 链）/ 预热 3s /
3 轮取中位数 / 各语言 Release 档。方法论与局限见
[benchmarks/BENCHMARK_FAIR.md](benchmarks/BENCHMARK_FAIR.md)。

| 框架 | plaintext RPS | JSON RPS | 内存(MB) | vs Zenith |
|---|---:|---:|---:|---:|
| **Zenith (C++17)** | **237,952** | **234,459** | **9.2** | **1.00×** |
| Rust (axum) | 153,043 | 153,218 | 7.1 | 0.64× |
| Go (gin) | 145,471 | 139,426 | 12.2 | 0.61× |
| Java (Spring Boot) | 133,369 | 123,164 | 156.5 | 0.56× |
| Java (JDK HttpServer) | 130,172 | 129,299 | 52.1 | 0.55× |
| Go (stdlib) | 128,422 | 116,097 | 8.4 | 0.54× |
| Node.js (node:http) | 88,537 | 89,017 | 48.2 | 0.37× |
| Node.js (fastify) | 85,217 | 83,211 | 57.5 | 0.36× |
| Node.js (express) | 60,078 | 55,990 | 51.9 | 0.25× |
| Python (FastAPI) | 3,902 | 2,993 | 53.6 | 0.02× |

> P99：除 Python（5–10ms）外全部 ≤1ms。内存：Spring Boot 的 **1/17**、Node 的 **1/6**。
> 局限：客户端与服务端共享同一物理机（loopback），更高速差需专用压测机验证。

### 静态文件零拷贝

| 文件大小 | 旧：整块缓冲 | 新：零拷贝路径 | 峰值 RSS（旧 → 新） |
|---|---:|---:|---:|
| 4 MB | ~700 MB/s | ~720 MB/s | 10 → 12 MB（持平） |
| 64 MB | **60 MB/s** | **1.05 GB/s** | **265 MB → 9.9 MB** |

文件越大差距越明显：旧路径每请求要拷三份完整文件，内存与耗时都随大小线性放大；
新路径恒定在 64KB 分块。Linux 上走 `sendfile` 还能再省掉用户态拷贝。

## 质量与可靠性

| 防线 | 现状 |
|---|---|
| 四平台构建 + 全量测试 | Windows / Linux(GCC) / Linux(Clang+协程) / macOS 全绿，**43 套** |
| Release 模式回归 | 全量测试在 Release 下再跑一遍（Debug 全绿 ≠ Release 全绿） |
| ASan + UBSan / TSan | 阻塞；**上线当轮抓到 6 个真问题**（含 macOS 段错误根因） |
| fuzz | 5 个解析入口（HTTP 请求/响应、WS 帧、JSON、WAV）白天冒烟 + 夜间 600s |
| 覆盖率 | **85.84%** 行覆盖（排除三方/测试/插件/示例），门限 80% |
| 性能门禁 | 64MB 文件峰值 RSS < 64MB + 吞吐下限（与硬件无关的行为不变量） |
| 消费者验证 | 装 SDK → `find_package(Zenith CONFIG)` → 编独立下游项目并运行 |
| 偶发失败检测 | 核心套件夜间重复 3 轮 |

未清零的两项（LSan 4 套件、macOS 2 套件）已转为 Issue 跟踪，细节见
**[docs/QUALITY.md](docs/QUALITY.md)**。

## 平台支持

| 平台 | 构建 | 测试 | 说明 |
|---|---|---|---|
| Windows · MinGW-w64 | ✅ | ✅ 43/43 | WSAPoll 后端；**发布包用它的产物** |
| Windows · MSVC | ✅ | — | 构建通过（`ZENITH_TLS=none`）；测试尚未接入 |
| Linux · GCC / Clang+Coro | ✅ | ✅ 43/43 | epoll 后端；Clang 任务额外跑协程层 |
| macOS · Apple Clang | ✅ | ⚠️ 41/43 | kqueue 后端；2 套 AI 编排用例跟踪中（[#3](https://github.com/Crush2680933378/Zenith/issues/3)） |

> **Windows 说明**：发布包由 MinGW-w64 构建（`libzenith.dll.a`）。MSVC 目前只验证到
> 「能编过」——导入库、测试、OpenSSL 均未接入；需要 MSVC 产出 `.lib` 请开 Issue。

## 文档导航

| 想了解 | 去哪 |
|---|---|
| 架构、模块划分、设计取舍 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 里程碑与每版实现明细 | [docs/MILESTONES.md](docs/MILESTONES.md) |
| 插件清单、热重载协议 | [docs/PLUGINS.md](docs/PLUGINS.md) |
| LLM 推理网关 / 引擎对接 | [docs/INFERENCE.md](docs/INFERENCE.md) |
| 质量防线（CI / sanitizer / fuzz） | [docs/QUALITY.md](docs/QUALITY.md) |
| Crew 事件契约（前端对接） | [docs/CREW_EVENTS.md](docs/CREW_EVENTS.md) |
| 语音（STT/TTS） | [docs/SPEECH.md](docs/SPEECH.md) |
| 依赖准入原则 | [docs/DEPENDENCIES.md](docs/DEPENDENCIES.md) |
| 性能基准方法论 | [benchmarks/BENCHMARK_FAIR.md](benchmarks/BENCHMARK_FAIR.md) |
| 路线图 | [PLAN_M7](docs/PLAN_M7.md) · [PLAN_M8](docs/PLAN_M8.md) |
| 变更记录 | [CHANGELOG.md](CHANGELOG.md) |

## 路线图

**M7 性能深挖 3/7**：✅ 内存优化（Context 池化 / 缓冲缩容）· ✅ 静态文件零拷贝 ·
✅ gzip；📋 热路径微优化（Linux eventfd 唤醒 / header 预编码）· io_uring · IOCP · HTTP/2。

**M8 生态扩展 5/8**：✅ 推理网关 · ✅ MCP · ✅ 向量库 · ✅ OTel + W3C Trace Context ·
✅ whisper STT；📋 MQTT broker 形态 · gRPC（需 HTTP/2 前置）· WebRTC。

剩余工作都在 [Issues](https://github.com/Crush2680933378/Zenith/issues) 里：
[#2](https://github.com/Crush2680933378/Zenith/issues/2) LSan 清零 ·
[#3](https://github.com/Crush2680933378/Zenith/issues/3) macOS 用例 ·
[#4](https://github.com/Crush2680933378/Zenith/issues/4) TSan 扩面 ·
[#5](https://github.com/Crush2680933378/Zenith/issues/5) M7 剩余 ·
[#6](https://github.com/Crush2680933378/Zenith/issues/6) M8 剩余。

## 参与贡献

- 环境、代码约定、提交规范、PR 检查清单 → [CONTRIBUTING.md](CONTRIBUTING.md)
- 安全问题请走私密渠道，攻击面与已知限制见 [SECURITY.md](SECURITY.md)

## 许可

Zenith 本体：MIT。uORM：MIT（`thirdparty/uORM`）。
