# 里程碑与实现明细

> 进展快照截至 2026-11-06（v0.2.2）。路线图见 [README · 路线图](../README.md#路线图)。

## 总览

| 里程碑 | 状态 | 一句话 |
|---|---|---|
| M1 插件内核 | ✅ | C ABI v1、服务注册表、线程池、日志、双示例插件 |
| M2 Reactor + HTTP | ✅ | 四后端 Poller、自研 HTTP/1.1、路由/中间件、自研 JSON |
| M3 数据层与协议 | ✅ | WebSocket、Redis RESP、JSON 配置、uORM 服务化 |
| M4 生态与边缘 | ✅ | TLS、FFmpeg、ESP32 设备桥、ComfyUI |
| M5 生产就绪 | ✅ | 静态文件/Session/优雅停机、指标、安全、SDK 导出 |
| M6 v0.1.0 发布 | ✅ | 181K RPS、连接上限、slowloris 防线、soak 1.64 亿零失败 |
| M7 性能深挖 | 🚧 3/7 | 内存优化 / 零拷贝 / gzip 已落地 |
| M8 生态扩展 | 🚧 5/8 | 推理网关、MCP、向量库、Trace Context、whisper |
| M9 流式与身份 | ✅ | SSE/chunked、gzip、RS/ES 非对称 JWT |
| M10 AI Agent 后端 | ✅ | ReAct、会话记忆、RAG、MCP 双向、HNSW |
| M11 多 Agent 集群 | ✅ | SDLC-Crew：7 角色、投票、产物库、缺陷环、SSE 状态板 |
| M12 多厂商 LLM 路由 | ✅ | 三协议适配器、模型发现、通配路由 |
| M13 全栈补强 | ✅ | ProviderRouter TLS 直连、guard、语音、cron、OTel、oauth、mqtt |
| M14 Crew 强化 | ✅ | 21 项：取消/恢复、成本、并行、记忆、辩论、评分卡 |
| M15 ZenithDesk 契约 | ✅ | 2.5D 可视化事件契约 + `/crew/ws` |
| M16 P0 护栏 | ✅ | 预算截断、成本熔断、cancel webhook、followup |
| M17 优化全包 | ✅ | SSE 定时器化、exec 白名单、去抖、淘汰、语义记忆… |
| M18 性能与生态收尾 | ✅ | 零拷贝、内存修正、Trace Context、whisper、macOS CI |
| M19 v0.2.0 发布 | ✅ | M9–M18 累积发布 |

## M1 — 插件内核

- `zenith/abi/zenith_abi.h` — 纯 C 插件 ABI v1（C89/C++ 双编译器可用，MSVC/GCC/Clang）
- `zenith::core` — Result / Buffer / string / time / KeyValueConfig
- `zenith::log` — LogSink 抽象、彩色控制台、MPSC 无锁异步滚动文件日志
- `zenith::concurrency` — ThreadPool、Vyukov 无锁有界队列
- `zenith::plugin` — DynLib、ServiceRegistry（接口名+版本协商）、PluginManager（安全卸载序）
- `zenith::coro` — 可选 C++20 协程层（`Task<T>` 惰性协程、对称转移、异常传播）
- `zenith/tls` — ITLSTransport 抽象接口
- 示例插件 ×2：`hello_cpp`（C++ 虚接口）、`hello_c`（纯 C 编译器构建）
- uORM（submodule）集成构建；自研 ZTEST 测试框架

## M2 — Reactor + HTTP

- `zenith::net` — `Poller` 抽象 + 四后端：**WSAPoll**(Windows) / **epoll**(Linux) /
  **kqueue**(macOS) / **select**(兜底)；`EventLoop`（min-heap 定时器、跨线程任务）；
  `EventLoopThreadPool`、`TcpServer`、`TcpConnection`（双 Buffer、写兴趣自适应）、`TcpClient`
- `zenith::http` — HTTP/1.1 全自研：增量解析器（任意切分、chunked、pipelining）、
  `Router`（`:param` / `*catchAll` / 分组 / 405）、Gin 风格 `Context`、洋葱中间件、
  `HttpServer` / `HttpClient`
- `zenith::json` — 自研 RFC 8259 解析/序列化（代理对、深度限制、最短往返双精度）
- 协程接入：`co_await tcpConnect(...)` / `co_await httpFetch(...)`

## M3 — 数据层与协议扩展

- `zenith::net::websocket` — RFC 6455 完整实现：帧编解码（掩码/16·64 位长度/分片）、
  会话（ping→pong、close 握手）、`HttpServer::enableWebSocket`、`WebSocketClient`
- `zenith::redis` — 自研 RESP2 客户端：增量解析、命令流水线、`commandBlocking`
- `zenith::config::JsonConfig` — dotted 嵌套取值、数组下标、flatten 成 KeyValueConfig
- `zenith::db` — uORM 服务化：`DbConfig` → `DataSource`（连接池）→ `IDbService`，
  阻塞操作经专用线程池，绝不占 EventLoop
- `zenith::crypto` — 自研 SHA-1 / Base64（标准向量 + Python 交叉核对）

## M4 — 生态与边缘

- `zenith::tls` — OpenSSL 后端：内存 BIO 非阻塞握手状态机、`listenTls` 一行 HTTPS、
  客户端 SNI/证书校验；自签证书端到端 + `curl --cacert` 实测
- `HttpClient` — URL 直填（http/https）+ keep-alive 连接池
- `media_ffmpeg` — probe / 任意时间点 JPEG 截图 / 转码管线（CRF/缩放/进度回调）
- `edge_esp32` — COBS+CRC32 帧协议、TCP 网关、串口双后端、ESP32 固件样例
- `ai_comfyui` — workflow 提交/占位符注入、/history 轮询、/view 产物下载
- `ports/esp-idf` — ESP-IDF 组件脚手架（静态注册表替代 dlopen，同一 C ABI）

## M5 — 生产就绪

- **Web 完整性**：multipart 上传、静态文件（Range/ETag/穿越防护）、Session/Cookie、
  优雅停机（在途排空 + 信号）、wss
- **可观测**：Prometheus 指标（Counter/Gauge/14 桶直方图）、X-Request-Id、`/zenith/status`
- **安全**：自研 SHA-256/HMAC（NIST 向量 + OpenSSL 交叉核对）、JWT、CORS、令牌桶限流、安全头
- **交付**：`find_package(Zenith CONFIG)` 安装导出、三平台 CI、`zenith-new` 脚手架
- **M5.7 实战沉淀**：`spaHandler`（真实文件优先，前端路由回 index.html，带扩展名 404 不伪装）；
  Windows 控制台 UTF-8；SDK 消费全流程验证；首个实战应用（`../ZenithBlog`）迁出

## M6 — v0.1.0

Release 基准 181K RPS、连接上限、slowloris 防线、soak 1.64 亿请求零失败、
Linux epoll 闭环、双平台发行版。

## M7 — 性能深挖（3/7）

| 项 | 状态 |
|---|---|
| M7.1 热路径微优化（eventfd 唤醒 / header 预编码） | 📋 |
| M7.2 内存优化（Context 池化、缓冲缩容、响应缓冲归还） | ✅ |
| M7.3 静态文件零拷贝（sendfile / 分块回退） | ✅ |
| M7.4 gzip 压缩 | ✅ |
| M7.5 io_uring / M7.6 IOCP | 📋 |
| M7.7 HTTP/2 | 📋 |

详见 [PLAN_M7.md](PLAN_M7.md)；实测数据见 [README · 性能](../README.md#性能)。

## M8 — 生态扩展（5/8）

- ✅ M8.1 推理网关 → [INFERENCE.md](INFERENCE.md)
- ✅ M8.2 插件热重载 → [PLUGINS.md](PLUGINS.md#插件热重载)
- ✅ M8.3 向量库（自研 HNSW + Qdrant API + 持久化）
- 📋 M8.4 MQTT broker / M8.5 gRPC / M8.6 WebRTC
- ✅ M8.7 OTel + W3C Trace Context
- ✅ M8.8 whisper.cpp STT → [SPEECH.md](SPEECH.md)

详见 [PLAN_M8.md](PLAN_M8.md)。

## M9 — 流式 / 压缩 / 非对称身份

- `http::SseStream` + `http::ChunkWriter`：`Context::sse()` 一行起流，事件帧由框架编码；
  写端线程安全，流式连接不参与 keep-alive 复用
- `http::gzipMiddleware`：`Accept-Encoding` 协商（含 `q=0` 拒绝）、体积阈值、204/304/HEAD 跳过
- `jwt::asym`：RS256/384/512、ES256/384（ECDSA 按 JWS 输出定长 `R‖S`，拒绝 `none`/`HS*`）

```cpp
// 一行起流：SSE 头由框架写，正文按 WHATWG EventSource 语义编码
server.router().get("/v1/chat/stream", [](http::Context& c) {
    auto sse = c.sse(200, {{"X-Model", "zenith-7b"}});           // 响应头此刻已发出
    sse->send(R"({"choices":[{"delta":{"content":"天顶"}}]})");   // data: <json>
    sse->send("1", "progress", "evt-1");                         // event + id
    sse->comment();                                              // 心跳，防反代超时
    sse->close();
});
// 推流不必发生在 handler 内：写端线程安全，可在任意工作线程继续 send()
```

## M10–M19 — AI 后端与协作引擎

这些里程碑的产出集中在 `crew/`、`ai/`、`vector/`、`mcp/` 模块：

- **M10**：ReAct 循环、会话记忆、RAG 召回、MCP 双向、自研 HNSW（Qdrant API）
- **M11 SDLC-Crew**：7 角色协作、投票决策（架构师加权）、产物库、缺陷环、人工门禁、SSE 状态板
- **M12 ProviderRouter**：openai-chat / anthropic-messages / gemini 三协议、模型发现、`$ENV:` 密钥
- **M13**：https 直连、三协议流式翻译、failover+熔断+成本、ai_guard、语音抽象、
  sched_cron、obs_otel、auth_oauth、net_mqtt 客户端
- **M14**：21 项强化（取消/恢复持久化、每角色模型、结构化自修正、真并行、组织记忆、辩论、
  评分卡、webhook、导出、写鉴权、受管线程池）
- **M15 ZenithDesk 契约**：`/crew/roster`、逐票 `vote_cast`、`handoff`、`/crew/ws`、
  `/crew/board` → [CREW_EVENTS.md](CREW_EVENTS.md)
- **M16 P0 护栏**：上下文预算截断、成本预算熔断、cancel webhook、followup 增量迭代
- **M17 优化全包**：SSE 定时器化、execVerify 白名单、持久化去抖、任务淘汰、语义记忆、
  guard 联动、定时 crew、WS 心跳/背压、投票配置化、分页、OTel 锁外上传
- **M18**：静态文件零拷贝、内存修正、W3C Trace Context、whisper STT、macOS CI
- **M19**：v0.2.0 发布（M9–M18 累积）

## 计划外

插件热重载（`PluginManager::reload` + `PluginWatcher`）→ [PLUGINS.md](PLUGINS.md#插件热重载)。
