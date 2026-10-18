# Zenith

[![CI](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml/badge.svg)](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

**天顶，顶点，万物之巅。** Zenith 是一款 C++17 高并发后端框架（对标 Java Spring Boot / Go Gin 的定位），
以**纯 C ABI 插件内核**为地基：一切扩展能力——数据库、TLS、FFmpeg 媒体、ComfyUI/AI、ESP32 与泰山派
RK3566/RK3576 边缘设备——都通过稳定 ABI 或可选编译模块接入。

前端交给 React，Zenith 只提供 API。

## 🏆 性能：跨语言公平基准（10 框架对比）

同机 loopback / 统一客户端 zenith-bench（2 线程×8 keep-alive 链）/ 预热 3s /
每端点 3 轮取中位数 / 各语言 Release 构建档。完整方法论与局限声明见
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

> P99：除 Python（5-10ms）外全部 ≤1ms。内存：Spring Boot 的 **1/17**、Node 的 **1/6**。
> 局限：客户端与服务端共享同一物理机（loopback），更高速差需专用压测机验证。

## M5（当前）：生产就绪

- **Web 完整性**：multipart 文件上传、静态文件服务（Range/ETag/穿越防护）、
  Session/Cookie（内存存储 + 中间件）、优雅停机（在途排空 + 信号处理）、wss
- **可观测**：Prometheus 指标（Counter/Gauge/14 桶直方图 + P50/99）、
  X-Request-Id 全链路、`/zenith/status` 运行时状态
- **安全**：自研 SHA-256/HMAC（NIST 向量 + OpenSSL 2000 组随机交叉核对）、JWT
  签发/校验（恒时比较）、CORS 白名单、令牌桶限流、安全响应头
- **性能**：zenith-bench 自研压测工具 + 31K RPS 基线（Debug/loopback）+
  CI 回归门槛
- **交付**：`find_package(Zenith CONFIG)` 安装导出、GitHub Actions 三平台 CI、
  zenith-new 工程脚手架

## M4：生态与边缘（TLS/媒体/AI/设备桥）

- `zenith::tls` —— OpenSSL 后端：内存 BIO 非阻塞握手状态机（TlsFilter 作为
  TcpConnection 加解密滤层，对 HTTP 层透明）、`HttpServer::listenTls` 一行 HTTPS、
  客户端 SNI/证书校验/自定 CA；自签证书端到端 + curl --cacert 实测
- `HttpClient` —— URL 直填（http/https 解析）+ keep-alive 连接池（同 host 复用、
  惰性剔除失效连接、归还/取用全在 loop 线程）
- `media_ffmpeg` 插件 —— FFmpeg probe/snapshot/transcode（COBS…不，libavformat 全套：
  探测元信息、任意时间点 JPEG 截图（sws 缩放+mjpeg）、转码管线（CRF/缩放/进度回调））
- `edge_esp32` 插件 —— COBS+CRC32 帧协议、TCP 网关（设备 hello/心跳/RPC/事件）、
  串口通道（win32/termios 双后端）、`zenith::io::SerialPort`；ESP32 固件样例
  （examples/05_esp32_firmware，ESP-IDF 裸 lwIP 实现同帧协议）
- `ai_comfyui` 插件 —— ComfyUI workflow 提交/占位符注入（{{seed}}/{{key}}）、
  /history 轮询、/view 产物下载；mock ComfyUI 全链路测试（自家 HttpServer 扮演）
- `ports/esp-idf` —— ESP-IDF 组件脚手架（静态注册表替代 dlopen，同一 C ABI）
- 测试：+tls/media/edge/ai 四套件，debug 17/17 全绿；aarch64 交叉留待工具链

## M3：数据层与协议扩展

- `zenith::net::websocket` —— RFC 6455 完整实现：帧编解码（掩码/16·64 位长度/分片重组）、
  会话（ping→pong 自动应答、close 握手、大消息上限）、`HttpServer::enableWebSocket` 升级路径、
  `WebSocketClient`（升级 + 帧流，客户端帧强制掩码）
- `zenith::redis` —— 自研 RESP2 客户端：增量解析器、命令流水线（回调 FIFO 对齐）、
  `commandBlocking` 便捷形态；测试用内置 mock Redis 服务器（无需真实 Redis）
- `zenith::config::JsonConfig` —— JSON 配置（自研解析器驱动）：dotted 嵌套取值、数组下标、
  flatten 成 KeyValueConfig（直接对接宿主 C ABI config_get）
- `zenith::db` —— uORM 服务化：`DbConfig`（从配置构建）→ `DataSource`（uORM 有界连接池）→
  `IDbService` 注册进 ServiceRegistry；DB 阻塞操作经专用线程池 `post()` 执行，绝不占 EventLoop
- `zenith::crypto` —— 自研 SHA-1 / Base64（WebSocket 握手；已用标准向量与 Python 权威核对）
- `zenith::coro` —— 通用回调→协程挂起器（callback_await.hpp）+ `co_await redisCommand(...)`
- 示例：`03_websocket`（广播 + REST 统计）
- 测试：+websocket/redis/config/db 四套件，sqlite 驱动真实建表查询；debug 13/13、coro 15/15

## M2：Reactor + HTTP

- `zenith::net` —— Reactor 网络层
  - `Poller` 抽象 + 四后端：**WSAPoll**(Windows) / **epoll**(Linux) / **kqueue**(macOS) / **select**(兜底)
  - `EventLoop`：fd 就绪回调、min-heap 定时器（可取消）、跨线程任务（自连接 UDP 唤醒）
  - `EventLoopThreadPool`（one loop per thread）、`TcpServer`（accept 分发）、`TcpConnection`
    （双 Buffer、写兴趣自适应、context 附带状态）、`TcpClient`（非阻塞连接 + 超时）
- `zenith::http` —— HTTP/1.1 全自研
  - 增量解析器（任意字节切分、Content-Length/chunked、pipelining、体积限制）
  - `Router` 路由树（`:param` / `*catchAll` / 静态优先 / 405 语义 / 分组）
  - `Context`（Gin 风格：param/query/header/bodyJson/text/json/abort/next）
  - 洋葱中间件、`HttpServer`（keep-alive/HEAD/异常 500/坏 JSON 400）、`HttpClient`（异步 + fetchBlocking）
- `zenith::json` —— 自研 RFC 8259 解析/序列化（代理对、深度限制、最短往返双精度、紧凑/美化）
- 协程接入：`syncWait` 升级为跨线程汇合；`co_await tcpConnect(...)` / `co_await httpFetch(...)`
- 示例：`02_http_server`（Gin 风格：访问日志中间件、路由参数、JSON API、鉴权分组、自请求演示）

## M1：插件内核

- `zenith/abi/zenith_abi.h` — 纯 C 插件 ABI v1（C89/C++ 双编译器可用，MSVC/GCC/Clang）
- `zenith::core` — Result / Buffer / string / time / KeyValueConfig
- `zenith::log` — LogSink 抽象、彩色控制台、MPSC 无锁异步滚动文件日志
- `zenith::concurrency` — ThreadPool、Vyukov 无锁有界队列
- `zenith::plugin` — DynLib（LoadLibrary/dlopen 统一封装）、ServiceRegistry
  （接口名+版本协商）、PluginManager（安全卸载序：stop → shutdown → 清服务 → dlclose）
- `zenith::coro` — 可选 C++20 协程层（`Task<T>` 惰性协程、对称转移、异常传播）
- `zenith/tls` — ITLSTransport 抽象接口（后端 openssl/mbedtls/wolfssl，M2/M3 实装）
- 示例插件 ×2：`hello_cpp`（C++ 虚接口服务）、`hello_c`（**纯 C 编译器**编译，验证纯 C ABI）
- uORM（git submodule）已集成构建（MySQL/PostgreSQL/SQLite 驱动按环境自动探测）
- 自研 ZTEST 微型测试框架，零三方依赖

## 快速开始

```bash
git clone <repo> && cd Zenith
git submodule update --init --recursive   # 拉取 uORM + uJSON

# Windows (MSYS2 MinGW-w64)
cmake --preset win-mingw-debug
cmake --build --preset win-mingw-debug
ctest --preset win-mingw-debug

# 协程层（C++20，可选编译）
cmake --preset win-mingw-coro
cmake --build --preset win-mingw-coro
ctest --preset win-mingw-coro

# 运行插件宿主示例
./build/win-mingw-debug/bin/plugin_host.exe
```

Linux：`linux-gcc-debug` / `linux-gcc-release` / `linux-clang-debug` 等 preset 同理；
交叉编译见 `cmake/toolchains/aarch64-linux-gnu.cmake`（泰山派 RK3566/RK3576）。

## 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `ZENITH_BUILD_STATIC` | ON | 静态库 `zenith_core`（嵌入式/单体部署） |
| `ZENITH_BUILD_SHARED` | ON | 动态库 `zenith`（符号经 ZENITH_API 显式导出） |
| `ZENITH_BUILD_TESTS` | ON | ZTEST 单测 + 插件端到端测试 |
| `ZENITH_BUILD_EXAMPLES` | ON | 示例（01_plugin_host 等） |
| `ZENITH_BUILD_PLUGINS` | ON | 示例插件 |
| `ZENITH_ENABLE_COROUTINES` | OFF | C++20 协程层 `zenith_coro`（编译期切换，不影响核心 C++17） |
| `ZENITH_ENABLE_ORM` | ON | 集成 thirdparty/uORM 子模块 |
| `ZENITH_TLS` | none | `openssl / mbedtls / wolfssl / none`（M2/M3 实装后端） |

## 写一个插件

```c
/* pure C 即可（或 C++，见 plugins/hello_cpp） */
#include "zenith/abi/zenith_abi.h"

static const zenith_host_api* g_host;

static zenith_status my_init(const zenith_host_api* host) {
    g_host = host;
    host->log_write(host->log_ctx, ZENITH_LOG_INFO, __FILE__, __LINE__, "my plugin up");
    return host->register_service(host->registry_ctx, host->plugin_token,
                                  "my.IService", 1, &my_instance, NULL);
}

static zenith_plugin_desc g_desc = {
    sizeof(zenith_plugin_desc), ZENITH_ABI_VERSION,
    "my-plugin", "0.1.0", "desc", "you",
    &my_init, NULL, NULL, NULL, {NULL, NULL, NULL, NULL},
};

ZENITH_EXPORT const zenith_plugin_desc* zenith_plugin_entry(void) { return &g_desc; }
```

宿主加载：`PluginManager::loadDirectory()` → `startAll()` → `registry.queryRaw("my.IService", 1)`。

## 里程碑

- **M1 ✅ 插件内核**：C ABI v1、服务注册表、插件管理、线程池、日志、双示例插件、uORM 集成
- **M2 ✅ Reactor + HTTP**：EventLoop（WSAPoll/epoll/kqueue/select 四后端）、TCP 全家、自研
  HTTP/1.1 解析器、路由/中间件/Context、HttpServer/HttpClient、自研 JSON、协程 co_await 接入
- **M3 ✅ 数据层与协议扩展**：WebSocket(RFC6455)、自研 Redis RESP 客户端、JSON 配置、
  uORM 服务化（连接池 + ServiceRegistry + 专用 DB 线程池）、SHA1/Base64
- **M4 ✅（主体）**：TLS/OpenSSL、FFmpeg 媒体、edge 设备桥、ComfyUI AI、URL/连接池；
  RK/ESP32 实机联调待硬件（脚手架与固件样例就绪）
- **后续**：uORM 服务化封装（连接池/事务）、WebSocket(RFC6455)、自研 Redis RESP 客户端、JSON 配置
- **M4 ✅（主体）**：TLS/OpenSSL、FFmpeg 媒体、edge 设备桥、ComfyUI AI、URL/连接池；
  RK/ESP32 实机联调待硬件（脚手架与固件样例就绪）
- **后续**：TLS 后端实装（OpenSSL/mbedTLS/wolfSSL）、media-ffmpeg、ai-comfyui、
  edge-esp32 插件、RK3566/RK3576 与 ESP-IDF 真机联调

## 依赖原则

能自研的全部自研（JSON/HTTP/EventLoop/日志/线程池/WebSocket/配置/Redis 协议）；
仅白名单允许三方：uORM（作者自有 MIT 库）、成熟 C ABI TLS 库、FFmpeg（M4 媒体插件）。

## 许可

Zenith 本体：MIT（待作者确认）。uORM：MIT（thirdparty/uORM）。
