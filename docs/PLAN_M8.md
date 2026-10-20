# Zenith M8 计划 —— 生态扩展（2026 最热技术方向 × Zenith 插件体系）

> 前置：M1–M6 完成（v0.1.0），M7 性能优化路线就绪。
> M8 目标：用 8 个新插件覆盖 2026 年最火的后端技术方向——AI 推理服务、
> MCP Agent 协议、向量数据库、MQTT 消息、gRPC、WebRTC 实时通信、
> OpenTelemetry 追踪、语音识别——使 Zenith 成为完整的 AI 时代后端底座。

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

**定位**：Zenith 成为高性能 AI 推理网关——封装 llama.cpp，暴露 OpenAI 兼容 API。

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

**接口设计**：
```cpp
class IInferenceService : public zenith::plugin::IService {
    ZENITH_SERVICE_DECL(IInferenceService, "zenith.ai.IInferenceService", 1)
    virtual Result<ModelHandle> loadModel(const ModelConfig&) = 0;
    virtual void chat(const ChatRequest&, ChatCallback) = 0;  // 异步流式
    virtual void embeddings(const std::string& text, EmbedCallback) = 0;
};
```

**预估**：~2000 行 C++（llama.cpp 封装 800 行 + OpenAI API 路由 600 行 +
SSE 流式 300 行 + 模型管理 300 行）

**为什么 Zenith 做最好**：C++ 性能（181K RPS HTTP 基线）+ llama.cpp 同为
C++ 生态 + C ABI 插件系统支持模型热插拔

---

### M8.2 `ai-mcp` — MCP (Model Context Protocol) 服务器插件 ⭐⭐⭐⭐⭐

**定位**：Zenith 作为 MCP Server——把数据库/Redis/文件/媒体/设备暴露为 AI Agent 可调用的工具。

**功能清单**：
- MCP 协议实现（JSON-RPC 2.0 over HTTP + SSE）
  - `initialize` / `tools/list` / `tools/call` / `resources/list` / `resources/read`
- **工具桥接**——把 Zenith ServiceRegistry 里的服务自动映射为 MCP 工具：
  - `zenith_db_query(sql)` → uORM 执行
  - `zenith_redis_command(cmd)` → Redis 客户端
  - `zenith_media_transcode(input, output)` → FFmpeg 插件
  - `zenith_device_rpc(device, method)` → edge_esp32 设备
  - `zenith_file_read(path)` / `zenith_file_write(path, content)` → 安全沙箱文件
- SSE 事件流（工具执行进度推送）
- 工具权限与沙箱（可配置白名单/黑名单/速率限制）

**架构**：
```
[AI Agent (LangGraph/CrewAI/Claude)] ←→ MCP 协议 ←→ [Zenith ai-mcp 插件]
                                                        ↓ ServiceRegistry
                                          [uORM] [Redis] [FFmpeg] [ESP32] [文件]
```

**预估**：~1200 行（MCP 协议栈 500 行 + 工具桥接 400 行 + SSE 200 行 + 配置 100 行）

**为什么 Zenith 做最好**：ServiceRegistry 就是现成的工具注册中心；JSON-RPC/SSE
底层协议 Zenith 已全部自研实现

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
  M8.1 ai-inference（llama.cpp + OpenAI API）     ← "推理之年"窗口
  M8.2 ai-mcp（MCP Server 工具桥）                  ← Agent 生态入口

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
| ai-inference | ~2000 | llama.cpp SDK |
| ai-mcp | ~1200 | 无（全自研） |
| db-vector | ~1500 | 无（全自研 HNSW） |
| net-mqtt | ~2500 | 无（全自研） |
| net-grpc | ~4000 | HTTP/2 (M7.7) |
| media-webrtc | ~3500 | OpenSSL DTLS / libsrtp |
| obs-otel | ~1000 | 无 |
| ai-stt | ~1200 | whisper.cpp SDK |
| **合计** | **~16,900** | — |

## 对 Zenith 生态的影响

M8 完成后 Zenith 将具备：

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
| **总计 17 个插件** | 覆盖 2026 后端全场景 |
