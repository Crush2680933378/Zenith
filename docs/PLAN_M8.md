# Zenith M8 计划 —— 生态扩展（2026 最热技术方向 × Zenith 插件体系）

> 前置：M1–M6 完成（v0.1.0），M7 性能优化路线就绪。
> M8 目标：用 8 个新插件覆盖 2026 年最火的后端技术方向——AI 推理服务、
> MCP Agent 协议、向量数据库、MQTT 消息、gRPC、WebRTC 实时通信、
> OpenTelemetry 追踪、语音识别——使 Zenith 成为完整的 AI 时代后端底座。

## 进度快照（2026-10-21）

| 项 | 计划 | 实际 |
|---|---|---|
| **M8.1** `ai-inference` | 1 个插件，~2,000 行 | ✅ **已实装，拆成 4 个插件**（AI 侧 ~3,470 行 C++） |
| **M8.2–M8.8** | 7 个插件 | 📋 **未开始** |
| **计划外** | — | 插件热重载（`PluginManager::reload` + `PluginWatcher`，169 行 + 测试） |
| **前置** | M7.4 压缩、HTTP 流式 | ✅ **已提前落地**（zlib 后端 + `SseStream`），M8.1 不再被卡 |

**M8.1 为什么变成 4 个插件**——原计划"封装 llama.cpp"在实机上遇到 C ABI 分野：

| 插件 | 形态 | 引擎有 C ABI？ |
|---|---|---|
| `ai_inference` | 通用网关：mock / 外部 OpenAI 上游 / 本地引擎进程监管 | — |
| `ai_llama` | **进程内** llama.cpp（`libllama.a` 8.5 MB 静态链入） | ✅ `llama.h` 全在 `extern "C"` 内，不透明句柄 |
| `ai_ninfer` | NInfer 引擎**子进程 + HTTP** | ❌ 发行版无头文件/导入库；能力只在 server 层 |
| `ai_ninfer_native` | NInfer **进程内**，自研纯 C ABI shim + 宿主适配器 | ⚠️ 我们自己补的 C ABI（MSVC 侧 550 MB DLL） |

实测（RTX 5070 Ti 16 GB / `Ternary-Bonsai-2-27B-ninfer-v3`）：
**prefill 3.65k tok/s、decode 729 tok/s、投机接受 89%**（DFlash2 d12）；
llama.cpp 侧：10.18 GB `Qwen3.8-27B-UD-IQ3_XXS.gguf` 加载 1.2 s、24 token 4.4 s（CPU）。
完整数据与复现命令见 [plugins/ai_ninfer_native/RESULTS.md](../plugins/ai_ninfer_native/RESULTS.md)。

## 市场判断依据（2026-10 搜索验证）

- **AI 推理**被 Coreweave/VAST Data 定义为"2026 推理之年"——推理工作负载
  增长 219% YoY（来源：Mirantis/Spheron/Braintrust 行业报告）
- **MCP（Model Context Protocol）**成为 AI Agent 生态标准协议——所有主流
  Agent 框架（LangGraph/CrewAI/AutoGen/OpenAI Agents SDK）通过 MCP 连接工具
- **向量数据库**因 RAG（检索增强生成）爆发——Qdrant/Milvus/Weaviate 增长
- **MQTT** 与 Zenith 已有 edge_esp32 插件天然配对
- **gRPC** 仍是微服务/AI 服务编排的标准通信协议

---

## 第一梯队：AI 基础设施（最高优先级）

### M8.1 `ai-inference` — LLM 推理服务插件 ⭐⭐⭐⭐⭐

> **状态（2026-10）：✅ 已实装（2026-10-21 重写为「进程监管」形态）。** 与本节原计划的偏差：
> 1. **OpenAI 兼容面下沉到核心库**（`zenith::ai::mountOpenAiApi`），插件只实现
>    `zenith.ai.IInferenceService` 并注册服务——路由不属于插件职责，与
>    media_ffmpeg/edge_esp32 的「插件提供能力、宿主装配 HTTP」同构。
> 2. **接口改为回调式**（`chat(req, onToken, onReasoning)` 同步返回 `ChatResult`），
>    不是原设计的 `loadModel/chat(cb)` 异步句柄：推理线程由宿主的线程池决定，
>    插件不持有推理线程，卸载时序因此简单（stop → 服务销毁）。
> 3. **进程内 llama.cpp 后端已删除，改为「监管本地引擎进程 + OpenAI 协议」。** 取证：
>    - 两套发行版（`llama.cpp-prism-b10709`、`infer-engine-sm120a-20261002`）都是
>      **纯二进制**，没有 `llama.h`、没有导入库；
>    - 源码树 `C:\APP\GitHubCode\llama.cpp` 是 **b10603**，二进制是 **b10709**，
>      跨 106 个 build，按值返回的 `llama_context_params` 布局无保证；
>    - `--kv-mean-center`（Prism）与 `--spec mtp|dflash2` / KVMem 分页（NInfer）
>      全在 **server 层**，公开 C API 里没有入口。
>
>    结论：这些引擎的能力在进程内、只认自己目录的 DLL，硬链既拿不到头文件也拿不到特性。
>    插件改为 `EngineSupervisor`（`CreateProcess`/`fork+exec` + 管道 + 就绪标记 + 崩溃重启 +
>    Job 对象回收）+ OpenAI 上游后端，并把引擎控制台统计行解析成遥测。
> 4. **实测（RTX 5070 Ti / sm_120a / NInfer PQ2-mtponly / 池 8192）**：
>    1000 token 出 —— **TTFT 340 ms、预填 3,340 tok/s、解码 282.8 tok/s、
>    投机接受 778/883（88%）**，总耗时 3.9 s；流式 SSE 11 片；引擎在插件卸载时被回收。
>    （同包在 4080 SUPER 上标称 3.52k / 278，同一量级。）
>
> 剩余待补：模型热插拔（换 `.ninfer` 需重启引擎）、多引擎/多卡路由、
> `logprobs`、WebSocket 通道（SSE 已通）、视觉输入（引擎已 `--vision`，网关侧未暴露）。

**定位（实际落地）**：Zenith 成为高性能 AI 推理网关——**监管本地引擎进程**（NInfer / llama-server）并暴露 OpenAI 兼容 API。

**功能清单**：
- llama.cpp C ABI 封装（模型加载/推理/SSE 流式 token 推送）
- **OpenAI 兼容 API**：
  - `POST /v1/chat/completions`（对话补全，支持 stream=true SSE）
  - `POST /v1/completions`（文本补全）
  - `POST /v1/embeddings`（向量嵌入）
  - `GET /v1/models`（模型列表）
- 多模型管理：同时加载 N 个模型（小请求→7B / 复杂请求→70B 路由）
- GPU/NPU 自动检测（CUDA/Metal/Vulkan 后端选择）
- 推理参数透传（temperature/top_p/max_tokens/stop/logprobs）
- token 用量统计 + Prometheus 指标暴露
- 连接池友好的长连接流式推送（WebSocket + SSE 双通道）

**架构**：
```
[Zenith HttpServer] → [ai_inference 插件] → [llama.cpp C ABI] → [GPU/CPU]
     /v1/chat/completions                    gguf 模型文件
```

**接口设计（实际落地）**：
```cpp
class IInferenceService : public zenith::plugin::IService {
    ZENITH_SERVICE_DECL(IInferenceService, "zenith.ai.IInferenceService", 1)
    virtual std::vector<ModelInfo> models() = 0;
    virtual ChatResult chat(const ChatRequest&, const TokenCallback& onToken = nullptr,
                            const TokenCallback& onReasoning = nullptr) = 0;
    virtual EmbeddingResult embeddings(const std::string& model, const std::string& input) = 0;
    virtual std::string backend() const = 0;        // "openai-upstream" / "mock"
    virtual EngineState engineState() const { return {}; }  // 引擎进程运行态
};
// 宿主侧：ai::mountOpenAiApi(router, service, {.prefix="/v1", .pool=&threadPool});
```

**预估**：~2000 行 C++（llama.cpp 封装 800 行 + OpenAI API 路由 600 行 +
SSE 流式 300 行 + 模型管理 300 行）

**为什么 Zenith 做最好**：C++ 性能（181K RPS HTTP 基线）+ llama.cpp 同为
C++ 生态 + C ABI 插件系统支持模型热插拔

---

### 计划外落地：引擎形态的四个插件（M8.1 衍生）

原计划 M8.1 只写"封装 llama.cpp 暴露 OpenAI API"。实机落地时发现**引擎的 C ABI 状况决定插件形态**，
于是拆成四个，并在框架层顺手补了热重载：

| 插件 | 服务名（可同进程共存） | 形态 | 重载代价 |
|---|---|---|---|
| `ai_inference` | `zenith.ai.IInferenceService` | 通用网关：mock / 外部 OpenAI 上游 / 本地引擎**进程监管** | 毫秒级 |
| `ai_llama` | `zenith.ai.ILlamaInference` | **进程内** llama.cpp，`libllama.a` 静态链入 | ~1 s（重 mmap 模型） |
| `ai_ninfer` | `zenith.ai.INinferInference` | NInfer **子进程 + OpenAI HTTP**（引擎无 C ABI） | ~4–6 s（重拉进程） |
| `ai_ninfer_native` | `zenith.ai.INinferCApi` | NInfer **进程内**，自研纯 C ABI shim（MSVC 侧 550 MB DLL） | ~3.5 s（重载 8.9 GB） |

**C ABI 取证**（决定前两条路）：

| 引擎 | 结论 | 依据 |
|---|---|---|
| llama.cpp | ✅ 纯 C ABI | `include/llama.h` 全在 `extern "C"` 内；`llama_model`/`llama_context` 不透明句柄 |
| NInfer | ❌ 无 C ABI | 发行版只有 `.exe`+DLL（无头文件/导入库）；上游明写 headers 不作为 SDK 分发；KVMem/投机解码只在 server 层 |

**为此新增的框架能力**：

- `zenith/ai/*`：推理服务契约、OpenAI 兼容层（SSE 流式 + usage）、NInfer C ABI 契约与宿主适配器
- `http::SseStream` / `ChunkWriter`、`http::gzipMiddleware`、`jwt::asym`（前置，已随本次交付）
- `PluginManager::unload/reload` + `PluginWatcher`（`.new` 交换协议）；Windows 下运行中的 DLL
  不可覆盖，故必须走"先落盘 `.new` 再换"。Linux 另注意 glibc 的 NODELETE 缓存坑
  （见 [README 热重载节](../README.md)）

**实测**：`Ternary-Bonsai-2-27B` 进程内 prefill **3.65k tok/s** / decode **729 tok/s**
（DFlash2 d12，投机接受 89%）；1000 token 出 0.86 s。数据见
[plugins/ai_ninfer_native/RESULTS.md](../plugins/ai_ninfer_native/RESULTS.md)。

### M8.2 `ai-mcp` — MCP (Model Context Protocol) 服务器插件 ⭐⭐⭐⭐⭐

> **规范基线：MCP `2026-07-28`（无状态核心）**，不是 2025-11-25 及更早的会话式协议。
> 逐条差异见 <https://modelcontextprotocol.io/specification/2026-07-28/changelog>。
> 旧写法（`initialize` 握手 + `Mcp-Session-Id` + HTTP+SSE 传输 + `resources/subscribe`）
> 实现出来即过时，以下清单已按新规范改写。

**定位**：Zenith 作为 MCP Server——把数据库/Redis/文件/媒体/设备暴露为 AI Agent 可调用的工具。

**协议层（无状态形态）**：
- 传输只做 **Streamable HTTP**：一个 POST 的响应体即 SSE 流（`text/event-stream`），
  客户端需发 `Accept: application/json, text/event-stream`；HTTP+SSE 旧传输已废弃
- **无握手、无会话**：不实现 `initialize` / `notifications/initialized`，也没有 `Mcp-Session-Id`。
  每个请求自带协议版本与能力——`_meta` 里的
  `io.modelcontextprotocol/protocolVersion`、`clientCapabilities`、`clientInfo`；
  服务端在结果 `_meta` 回 `serverInfo`；版本不符回 `UnsupportedProtocolVersion`
- **`server/discover`（MUST）**：声明支持的协议版本、capabilities 与身份
- 列表/读方法：`tools/list`、`prompts/list`、`resources/list`、`resources/read`、
  `resources/templates/list`，结果必须带 `ttlMs` + `cacheScope`（`CacheableResult`）；
  `tools/list` 需**稳定排序**（提升 LLM prompt cache 命中率）
- 变更通知统一走 **`subscriptions/listen`**（一条长驻 POST 响应流），取代旧 GET 端点与
  `resources/subscribe` / `resources/unsubscribe`；通知打 `io.modelcontextprotocol/subscriptionId`。
  请求级通知（`notifications/progress` / `notifications/message`）仍走该请求自己的响应流
- **MRTR（多轮请求）**：不再发 server→client 主动请求。需要补输入时返回
  `resultType: "input_required"` + `inputRequests`，客户端带 `inputResponses` 重试原请求；
  所有结果都带 `resultType`（`complete` / `input_required`）
- Streamable HTTP POST 必须校验 **`Mcp-Method` / `Mcp-Name`** 头；工具参数可映射自定义头（`x-mcp-header`）
- 错误码分区：实现自定义用 `-32000..-32019`；规范保留 `-32020..-32099`
  （HeaderMismatch `-32020`、MissingRequiredClientCapability `-32021`、
  UnsupportedProtocolVersion `-32022`）；资源不存在由 `-32002` 改为 `-32602`
- **明确不做**：SSE `Last-Event-ID` 续传（规范移除，断流由客户端重发新请求）；
  Roots/Sampling/Logging（已废弃，日志级别改为每请求 `_meta.logLevel`）

**工具桥接（Zenith 独有优势）**：
- `tools/list` 由 **ServiceRegistry 自动生成**：注册进来的服务直接映射为工具 schema
  - `zenith_db_query` → uORM 执行
  - `zenith_redis_command` → Redis 客户端
  - `zenith_media_transcode` → FFmpeg 插件
  - `zenith_device_rpc` → edge_esp32 设备
  - `zenith_file_read` / `zenith_file_write` → 沙箱根目录白名单
- 工具级 ACL + 令牌桶限流；危险工具（执行类）走 `agent_sandbox`（M9 候选）隔离

**可观测与鉴权**：
- `_meta` 里的 W3C `traceparent` / `tracestate` / `baggage` 全链路透传（与 `obs_otel` 共用）
- Authorization 按规范做 OAuth 2.1 资源服务器：校验 RFC 9207 `iss`；
  客户端注册改用 **Client ID Metadata Documents**（DCR 已废弃）

**依赖**：零三方——HTTP/1.1、JSON-RPC、SSE 全在 Zenith 内
（SSE/分块写端已随 M9.1 落地：`zenith::http::SseStream`）

**预估**：~1500 行（协议栈 600 + 工具桥接 500 + SSE 帧 200 + 鉴权/配置 200）

**为什么 Zenith 做最好**：ServiceRegistry 就是现成的工具注册中心；JSON-RPC/SSE
底层协议 Zenith 已全部自研实现；无状态化后正好吃满 181K RPS 的 HTTP 基线，不再需要会话表

---

### M8.3 `db-vector` — 向量数据库插件 ⭐⭐⭐⭐⭐

**定位**：RAG 基础设施——自研 HNSW 向量索引 + Qdrant 兼容 REST API。

**功能清单**：
- **HNSW 索引**（分层可导航小世界图——当前最优 ANN 算法）
  - 参数：M=16, ef_construction=200, ef_search=50（可调）
  - 距离度量：cosine / dot-product / inner-product / euclidean
- 向量 CRUD：`/collections/{name}/points`（upsert/get/delete）
- 相似度搜索：`/collections/{name}/points/search`（top-K + 分数过滤）
- 集合管理：`/collections`（create/delete/list，支持维度/度量配置）
- 与 uORM 共存：向量数据可持久化到 SQLite（序列化 HNSW 图）
- 批量导入（`/points/batch`，支持 JSONL）
- 过滤搜索（标量过滤 + 向量搜索组合）

**REST API 兼容 Qdrant 格式**——现有 Qdrant 客户端零改动接入。

**预估**：~1500 行（HNSW 核心 800 行 + REST API 400 行 + 持久化 300 行）

**为什么 Zenith 做最好**：纯 C++ HNSW 查询 <1ms；自研约 800 行，零三方依赖

---

## 第二梯队：协议与通信

### M8.4 `net-mqtt` — MQTT 消息代理插件 ⭐⭐⭐⭐

**定位**：IoT 消息标准协议——与 edge_esp32 插件天然配对。

**功能清单**：
- MQTT 3.1.1 / 5.0 broker
  - QoS 0/1/2（至多一次/至少一次/恰好一次）
  - 保留消息（retained message）
  - 遗嘱消息（Last Will and Testament）
  - 会话保持（persistent session / clean start）
  - 共享订阅（$share/group/topic——负载均衡）
- Topic 通配符路由（`+` 单层 / `#` 多层）——复用 Zenith 路由树设计
- MQTT over WebSocket（浏览器直连）
- 与 edge_esp32 设备桥联动（设备消息自动路由到 MQTT topic）
- Prometheus 指标（连接数/消息吞吐/QoS 分布）
- ACL 权限控制（topic 级读/写权限）

**预估**：~2500 行（MQTT 协议栈 1500 行 + 会话管理 500 行 + 路由 500 行）

---

### M8.5 `net-grpc` — gRPC 服务框架插件 ⭐⭐⭐

**定位**：微服务/AI 服务编排标准通信协议。

**功能清单**：
- HTTP/2 帧解析器（与 M7.7 共享代码）
- HPACK 头压缩（静态表 + 动态表 + 霍夫曼编码）
- Protobuf wire format 编解码（自研，支持 proto3）
- 四种流模式：
  - Unary（请求-响应）
  - Server streaming（服务端推流）
  - Client streaming（客户端推流）
  - Bidirectional streaming（双向流）
- `grpc_health_check` 健康检查标准
- `grpc_reflection` 服务发现
- 拦截器（interceptor）中间件

**预估**：~4000 行（HTTP/2+HPACK 2000 行 + Protobuf 1000 行 + gRPC 语义 1000 行）

---

### M8.6 `media-webrtc` — WebRTC 实时通信插件 ⭐⭐⭐

**定位**：视频会议/直播/AI 实时视频通话的核心协议。

**功能清单**：
- ICE/STUN/TURN（NAT 穿透）
- DTLS 握手（可复用 OpenSSL DTLS）
- SRTP 加密媒体流
- 信令服务器（WebSocket 已有，SDP offer/answer 交换）
- 与 media_ffmpeg 联动：WebRTC 输入 → FFmpeg 转码 → WebRTC 输出
- 数据通道（DataChannel）
- 统计信息（RTCP receiver report / sender report）

**预估**：~3500 行（ICE/STUN 1000 行 + DTLS/SRTP 1000 行 + SDP 500 行 + 集成 1000 行）

---

## 第三梯队：运维与语音

### M8.7 `obs-otel` — OpenTelemetry 分布式追踪插件 ⭐⭐⭐

**功能**：
- OTLP 协议导出（gRPC + HTTP 两种传输）
- W3C Trace Context 头解析（`traceparent` / `tracestate`）
- Span 管理（创建/属性/事件/状态/跨服务传播）
- 与现有 X-Request-Id 中间件贯通（Request-Id ↔ trace-id 映射）
- 与 Prometheus 指标联动（同维度标签）

**预估**：~1000 行

---

### M8.8 `ai-stt` — 语音识别插件 ⭐⭐⭐

**功能**：
- 封装 whisper.cpp（同 C++ 生态）
- 实时流式 STT（WebSocket 音频帧推送 → 部分转写结果流式返回）
- 批量转写（HTTP 上传音频文件 → 完整转写文本）
- 多语言支持（whisper 模型自带 99 语言）
- 与 ai-inference 联动（语音输入 → STT → LLM → 语音输出链路）
- VAD（Voice Activity Detection）静音检测

**预估**：~1200 行

---

## 实施顺序与依赖

```
第一批（1-2 周/插件）：
  M8.1 ai-inference（llama.cpp + OpenAI API）     ← ✅ 已完成（拆成 4 个插件，见「进度快照」）
  M8.2 ai-mcp（MCP Server 工具桥）                  ← Agent 生态入口（下一项，前置已齐：SSE + 无状态 JSON-RPC）

第二批（2-3 周/插件）：
  M8.3 db-vector（HNSW 向量搜索）                   ← RAG 基础设施
  M8.4 net-mqtt（MQTT broker）                      ← IoT 消息闭环

第三批（长期）：
  M8.5 net-grpc（需 HTTP/2 先行）                    ← 依赖 M7.7
  M8.6 media-webrtc                                  ← 独立
  M8.7 obs-otel                                      ← 独立
  M8.8 ai-stt                                        ← 依赖 M8.1 可选
```

## 与 M7 性能优化的关系

M8 插件可与 M7 并行开发：
- ai-inference 推理网关受益于 M7.2 Context 池化（高频 API 调用）
- db-vector HNSW 查询受益于 M7.1 NUMA 绑核（内存局部性）
- net-mqtt 消息路由受益于 M7.3 零拷贝（大量小消息）
- M7.7 HTTP/2 是 M8.5 gRPC 的前置

## 代码量估算

| 插件 | 新增行数 | 依赖 |
|---|---|---|
| ai-inference | 计划 ~2000 → **实际 ~3470** | llama.cpp SDK + NInfer（4 个插件，含自研 C ABI shim） |
| ai-mcp | ~1500 | 无（全自研；协议基线 MCP 2026-07-28） |
| db-vector | ~1500 | 无（全自研 HNSW） |
| net-mqtt | ~2500 | 无（全自研） |
| net-grpc | ~4000 | HTTP/2 (M7.7) |
| media-webrtc | ~3500 | OpenSSL DTLS / libsrtp |
| obs-otel | ~1000 | 无 |
| ai-stt | ~1200 | whisper.cpp SDK |
| **合计** | **~16,900** | — |

## 对 Zenith 生态的影响

**当前（2026-10-21）**：已实装 9 个插件（5 个原有 + M8.1 衍生的 4 个）+
插件热重载 + 流式/压缩/非对称 JWT。下表是 M8 全部完成后的目标态：

| 能力域 | 覆盖 |
|---|---|
| AI 推理 | llama.cpp LLM 服务 + OpenAI API |
| AI Agent | MCP 协议服务器 |
| 向量搜索 | 自研 HNSW + Qdrant API |
| IoT 消息 | MQTT broker + ESP32 设备桥 |
| 微服务通信 | gRPC + HTTP/2 |
| 实时通信 | WebRTC + FFmpeg 转码 |
| 可观测 | Prometheus + OpenTelemetry |
| 语音 | whisper.cpp STT |
| **总计 13 个插件** | 覆盖 2026 后端全场景（现已实装 9 个，M8.2–M8.8 七项待做） |
