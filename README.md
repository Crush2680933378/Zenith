# Zenith

**天顶，顶点，万物之巅。** Zenith 是一款 C++17 高并发后端框架（对标 Java Spring Boot / Go Gin 的定位），
以**纯 C ABI 插件内核**为地基：一切扩展能力——数据库、TLS、FFmpeg 媒体、ComfyUI/AI、ESP32 与泰山派
RK3566/RK3576 边缘设备——都通过稳定 ABI 或可选编译模块接入。

前端交给 React，Zenith 只提供 API。

## M1（当前）：插件内核

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
- **M2 Reactor + HTTP**：EventLoop（epoll/kqueue/IOCP/select 四后端）、自研 HTTP/1.1 解析器、
  路由/中间件、自研 JSON；协程 awaiter 接入事件循环（`co_await http_fetch()`）
- **M3 数据层**：uORM 服务化封装（连接池/事务）、WebSocket(RFC6455)、自研 Redis RESP 客户端、JSON 配置
- **M4 生态与边缘**：TLS 后端实装（OpenSSL/mbedTLS/wolfSSL）、media-ffmpeg、ai-comfyui、
  edge-esp32 插件、RK3566/RK3576 与 ESP-IDF 真机联调

## 依赖原则

能自研的全部自研（JSON/HTTP/EventLoop/日志/线程池/WebSocket/配置/Redis 协议）；
仅白名单允许三方：uORM（作者自有 MIT 库）、成熟 C ABI TLS 库、FFmpeg（M4 媒体插件）。

## 许可

Zenith 本体：MIT（待作者确认）。uORM：MIT（thirdparty/uORM）。
