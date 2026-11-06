# 插件体系

Zenith 的一切扩展能力都走**纯 C ABI**：宿主只认 `zenith_plugin_desc` + 服务注册表，
跨编译器（MSVC/GCC/Clang）、跨 C++17/20 混编、跨二进制分发都不承担 C++ ABI 兼容义务。

- 写一个插件的最小骨架见 [README · 插件体系](../README.md#插件体系)
- 依赖准入规则见 [DEPENDENCIES.md](DEPENDENCIES.md)

## 已实装插件（16 个，含 2 个示例）

| 插件 | 功能 | 技术 |
|---|---|---|
| `hello_cpp` | C++ 虚接口服务示例 | 纯 C ABI |
| `hello_c` | **纯 C 编译器**构建的示例插件 | C89 兼容 |
| `media_ffmpeg` | probe / 截图 / 转码（进度回调） | FFmpeg SDK |
| `edge_esp32` | COBS 帧设备桥（TCP 网关 + 串口） | 自研协议 |
| `ai_comfyui` | ComfyUI workflow 提交 + 产物下载 | 自研 HTTP |
| `ai_inference` | 通用 LLM 网关：mock / 外部 OpenAI 上游 + SSE 流式 → [INFERENCE.md](INFERENCE.md) | 纯 C ABI 插件 |
| `ai_llama` | **进程内 llama.cpp**（真·C ABI 静态链接，可加载本地 GGUF） | `llama.h` C ABI |
| `ai_ninfer` | **NInfer 引擎**：进程监管 + OpenAI HTTP（引擎无 C ABI） | 子进程 + HTTP |
| `ai_ninfer_native` | **NInfer 进程内**：自研纯 C ABI shim（MSVC 侧编进 550 MB DLL） | 自研 C ABI |
| `db_vector` | **向量存储**：自研 HNSW（启发式选邻/多层图/墓碑删除）+ Qdrant 兼容 API + **原子快照持久化** | 纯 C ABI 插件 |
| `ai_mcp` | **MCP 网关**：JSON-RPC over HTTP，把向量库/推理自动桥接为 Agent 工具 | 纯 C ABI 插件 |
| `ai_agent` | **Agent 运行时**：ReAct 工具循环、会话滚动记忆、RAG 召回注入、MCP 远端工具 | 纯 C ABI 插件 |
| `ai_router` | **多厂商 LLM 路由**：三协议适配器、https/TLS 直连、流式 SSE→token、failover+熔断+成本记账 | 纯 C ABI 插件 |
| `ai_crew` | **多 Agent SDLC 集群**：7 角色 + 投票辩论/缺陷环/组织记忆/断点恢复 + SSE·WS → [CREW_EVENTS.md](CREW_EVENTS.md) | 纯 C ABI 插件 |
| `ai_guard` | **LLM 输入护栏**：提示注入短语检测 + PII 扫描（邮箱/手机/身份证/银行卡 Luhn/API Key）与掩码 | 纯本地规则 |
| `ai_speech` | **语音服务**：STT/TTS 接口 + 可播 WAV mock；whisper.cpp 真后端 → [SPEECH.md](SPEECH.md) | 纯 C ABI 插件 |

## 计划中

| 插件 | 功能 | 优先级 |
|---|---|---|
| `net_mqtt` | 🚧 客户端已入核心（3.1.1）/ broker 形态（保留消息/共享订阅）未做 | ⭐⭐⭐⭐ |
| `net_grpc` | gRPC（HTTP/2 + Protobuf + 四种流模式） | ⭐⭐⭐ |
| `media_webrtc` | WebRTC（ICE/DTLS/SRTP + FFmpeg 转码联动） | ⭐⭐⭐ |
| `obs_otel` | ✅ 已入核心（OTLP 导出 + W3C traceparent 透传） | — |
| `ai_stt` | ✅ 已实装（whisper.cpp 真后端） | — |

完整方案（功能/架构/代码量/依赖逐项标注）见 [PLAN_M8.md](PLAN_M8.md)。

## 插件热重载

`PluginManager` 支持**单个插件**卸载/重载，`PluginWatcher` 负责盯文件：

```cpp
plugin::PluginWatcher watcher(mgr, "./plugins", /*intervalMs=*/1000);
watcher.onEvent([](const plugin::PluginWatcher::Event& e) {
    if (!e.ok) ZLOG_ERROR("reload failed: ", e.message);
});
watcher.start();     // 后台轮询
watcher.pollOnce();  // 或手动触发一轮（测试/CI 用）
```

**Windows 为什么必须走 `.new`**：运行中的 DLL 不能覆盖。换版本的正确姿势是
「先把新文件落成 `foo.dll.new`」——watcher 检测到就：卸载旧插件 → 把旧文件挪开 →
把 `.new` 改名成正式文件 → 重新加载并 `start()`。失败则回滚，宿主不受影响。

| 能力 | 接口 |
|---|---|
| 单插件卸载 | `mgr.unload(name)`（stop → 清注册表 → shutdown → dlclose） |
| 单插件重载 | `mgr.reload(path)`：卸载后重新 `load + init + start` |
| 热替换 | `PluginWatcher::pollOnce()` / `start()`（`.new` 交换协议） |
| 改配置生效 | 同样用 `mgr.reload(path)`——重载会重新 `init()`，插件重新读 config |

**Linux 上的 NODELETE 坑**：glibc 的 `dlopen` 按路径缓存。若插件含 `STB_GNU_UNIQUE`
符号（C++ 内联静态量很常见），`dlclose` 后模块仍会被驻留，**再次 `dlopen` 同一路径直接
返回旧句柄，新文件根本不会被读取**——表现为"热替换成功但跑的还是旧代码"。
确定要热替换就用带版本号的文件名（`foo.so.v2`），别覆盖同名文件。

**两条必须知道的规则**：

1. **重载后旧服务指针失效**（旧实例已 destroy），消费者必须重新 `queryRaw`；
2. 重载代价 = 插件 `init()` 的代价，AI 插件差别很大：

| 插件 | 重载代价 | 说明 |
|---|---|---|
| `hello_cpp` / `hello_c` / `media_ffmpeg` / `edge_esp32` | 毫秒级 | 无重量级状态 |
| `ai_inference` / `ai_comfyui` | 毫秒级 | mock/上游形态，仅重建 HTTP 客户端 |
| `ai_llama` | ~1 s | 释放并重新 mmap 模型 |
| `ai_ninfer`（子进程） | ~4–6 s | 引擎进程被杀掉重拉 |
| `ai_ninfer_native`（进程内） | ~3.5 s | 引擎析构 + 重新加载 8.9 GB 权重 |

回归：`test_plugin_reload`（3 例）——`.new` 交换后服务被重注册且可用；坏文件不崩宿主；
真 GGUF（`Qwen3.8-27B-IQ3_XXS`）卸载后重载仍能推理。

**配置文件变了也想让插件重新读配置**，不用重启宿主：

```cpp
config::HotReloader cfgFile("./zenith.json", 500);
cfgFile.onChange([&](auto) {                 // 内容哈希变化才触发
    for (const auto& p : mgr.plugins()) mgr.reload(p->path);   // 重新 init() → 重新读 config
});
cfgFile.start();
```

> AI 插件重载会连模型/引擎一起重建（见上表），配置这类"高频小改"更建议做成
> 每请求读取而非重载插件。
