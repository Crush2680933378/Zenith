# Zenith

[![CI](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml/badge.svg)](https://github.com/Crush2680933378/Zenith/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)]()
[![Platforms](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey.svg)]()

**天顶，顶点，万物之巅。** Zenith 是一款 C++17 高并发后端框架（对标 Java Spring Boot / Go Gin 的定位），
以**纯 C ABI 插件内核**为地基：一切扩展能力——数据库、TLS、FFmpeg 媒体、**LLM 推理网关**
（进程内 llama.cpp / NInfer 引擎 / OpenAI 兼容上游）、ComfyUI 绘画、ESP32 与泰山派
RK3566/RK3576 边缘设备——都通过稳定 ABI 或可选编译模块接入。

前端交给 React，Zenith 只提供 API。

## 📌 进度总览（2026-10-21）

| 里程碑 | 状态 | 关键产出 |
|---|---|---|
| **M1–M6** | ✅ 完成 | v0.1.0：C ABI 插件内核 / Reactor / HTTP / WS / TLS / 生产就绪（181K RPS） |
| **M7 性能深挖** | 📋 计划中 | [PLAN_M7](docs/PLAN_M7.md)；其中 **M7.4 压缩已提前落地**（zlib 后端） |
| **M8 生态扩展** | 🚧 **2/8 实装** | ✅ M8.1 推理网关(4 插件)；✅ M8.2 MCP **+ M8.3 向量库提前落地**（M10 三件套：ai_mcp/db_vector/ai_agent）→ [PLAN_M8](docs/PLAN_M8.md) |
| **M10 AI Agent 后端** | ✅ 完成 | **推理+编排+工具协议+记忆检索成环**：ReAct 循环/会话记忆/RAG 召回、MCP 双向、HNSW 自研(Qdrant API)、示例 08 全链路 |
| **M9 流式与身份** | ✅ 完成 | SSE/chunked 流式、gzip 压缩、RS/ES 非对称 JWT |
| **计划外** | ✅ 完成 | 插件热重载（`PluginManager::reload` + `PluginWatcher`） |
| **CI** | ✅ 三平台绿 | Windows-MinGW / Linux-GCC / Linux-Clang+Coro，29–30 套测试 |
| **插件** | 9 个 | hello_cpp·hello_c·media_ffmpeg·edge_esp32·ai_comfyui·**ai_inference·ai_llama·ai_ninfer·ai_ninfer_native** |

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

## M8.1：LLM 推理网关（`ai_inference` + 进程监管 + OpenAI 兼容层）

> 2026-10 重写：**进程内链接 llama.cpp 的那条路已删掉**，改成「监管本地引擎进程 + 说 OpenAI 协议」。
> 依据见本节的引擎对接小节。

- **`ai_inference` 插件**（纯 C ABI，服务 `zenith.ai.IInferenceService` v1）三种形态：
  | 形态 | 做什么 |
  |---|---|
  | `engine` | **拉起并监管**本地引擎进程（NInfer / llama-server…），等日志就绪后转 OpenAI 协议对接；插件卸载时回收进程 |
  | `openai` | 接已在跑的外部 OpenAI 兼容服务（vLLM / 云端 API / 你手动起的引擎） |
  | `mock` | 确定性后端：无模型也能跑通全部契约（CI 用的就是它） |
- **`EngineSupervisor`**：`CreateProcess`/`fork+exec` + stdout/stderr 管道 + 就绪标记探测 +
  崩溃重启（限次）+ Job 对象（Windows `KILL_ON_JOB_CLOSE`，宿主退出不留孤儿占显存）+
  **解析引擎控制台统计行**（`TTFT / prefill / decode / mtp accepted`）
- **`zenith::ai::mountOpenAiApi(router, service, options)`**：OpenAI 兼容面在核心库，插件只管推理
  - `GET /v1/models`、`POST /v1/chat/completions`（`stream=true` 走 SSE）、
    `POST /v1/completions`、`POST /v1/embeddings`、`GET /zenith/ai/status`
  - 参数域收敛、OpenAI 形状错误体、`reasoning_content` 分离透出（思考模型）
  - **推理绝不占 EventLoop**：给 `options.pool` 就投递线程池；非流式用 chunked 延迟响应
- **引擎遥测进 Prometheus**：`zenith_ai_decode_tps`、`zenith_ai_prefill_tps`、`zenith_ai_ttft_ms`、
  `zenith_ai_draft_tokens_total`、`zenith_ai_draft_accepted_total`（投机解码接受率）、
  `zenith_ai_engine_running`、`zenith_ai_cached_prompt_tokens_total`
- 回归：`test_ai_inference`（11 例：契约 9 例 + 上游/引擎两个活体用例，未设环境变量自动跳过）

```cpp
// 宿主侧装配：加载插件 → 取服务 → 挂 OpenAI 路由（三行）
plugin::PluginManager mgr(registry, &pool, &cfg);
(void)mgr.loadDirectory("./plugins");
auto* svc = static_cast<ai::IInferenceService*>(
    registry.queryRaw(ai::IInferenceService::service_name(), 1));
ai::mountOpenAiApi(server.router(), *svc, ai::OpenAiApiOptions{"/v1", &pool});
server.listen("0.0.0.0", 8080);   // 之后 curl / OpenAI SDK 直接指过来
```

### 为什么不做进程内 llama.cpp

| 取证 | 结论 |
|---|---|
| `llama.cpp-prism-b10709` 与 `infer-engine-sm120a` 都是**纯二进制发行版** | 没有 `llama.h`、没有 `.lib`/`.dll.a`，无法编译链接 |
| 源码树 `C:\APP\GitHubCode\llama.cpp` 是 **b10603**，二进制是 **b10709** | 跨 106 个 build，`llama_context_params` 这类按值返回的结构体布局无保证 |
| `--kv-mean-center` / `--spec mtp|dflash2` / KVMem 分页 | 全在 **server 层**，公开 C API 里没有对应入口 |

结论：引擎的能力在进程内、只认自己的目录，硬链接既拿不到头文件也拿不到这些特性。
正确形态是「宿主拉进程 + 说 OpenAI 协议」，插件负责生命周期与遥测。

### 对接 NInfer 引擎（`infer-engine-sm120a-20261002`）

该包是 **NInfer** 引擎：OpenAI 兼容 HTTP、`.ninfer` 权重、KVMem 分页、MTP/dflash2 投机解码。
`backend=engine` 时插件按配置拉起它，并用 `--host/--port` 自动推出上游地址：

```json
{
  "ai.inference.backend": "engine",
  "ai.inference.engine.exe": "C:\\...\\infer-engine-sm120a-20261002\\engine\\ninfer-serve-120a.exe",
  "ai.inference.engine.model": "C:\\...\\models\\Ternary-Bonsai-2-27B-ninfer-v3-mtponly.ninfer",
  "ai.inference.engine.args": "<MODEL> --host 127.0.0.1 --port 8111 --model-id qwen3.8-27b --max-context 32768 --kv-capacity 8192 --kv-dtype k8v4 --host-kv-mib 4096 --prefill-chunk 1024 --spec mtp --draft-tokens 4 --default-max-tokens 32768 --default-reasoning-effort none --max-concurrency 1 --max-shared-prefixes 0 --presence-penalty 0 --temperature 0.7 --top-p 0.9 --top-k 20",
  "ai.inference.engine.env": "NINFER_KV_WINDOW=16384;NINFER_KV_RETRIEVE=8192;NINFER_KV_RING=1;NINFER_HOST_PAGEABLE=1;NINFER_KV_REUSE_HOSTBACKED=1",
  "ai.inference.engine.ready_line": "listening on",
  "ai.inference.default_model": "qwen3.8-27b"
}
```

实测（RTX 5070 Ti / sm_120a / PQ2-mtponly / 池 8192）：

```
[ engine ] pid=44076 running=1 last=... INFO api base http://127.0.0.1:8111/v1
[ status ] backend=openai-upstream models=["qwen3.8-27b"]
[ engine ] 测速：3887 ms 总耗时，正文 1000 字节：301 302 303 ...
[ engine ] 控制台：reqs=2 ttft=340ms prefill=3340 decode=282.8 acc=778/883
[ engine ] 流式分片 11 个，末片=[DONE]
```

即 1000 token 出：**TTFT 340 ms / 预填 3,340 tok/s / 解码 282.8 tok/s / 投机接受 88%**——
与该包在 4080 SUPER 上标称的 3.52k / 278 同一量级。引擎在插件卸载时被回收，显存回到基线。

> 思考模型提醒：`max_tokens` 小于思考长度时会只剩 `reasoning_content`、`content` 为空；
> 网关把两者分开透出（`message.reasoning_content` / `delta.reasoning_content`）。


### 两个引擎插件：C ABI 分野（2026-10-21）

| | `ai_llama` | `ai_ninfer` |
|---|---|---|
| **C ABI** | ✅ `llama.h` 全部在 `extern "C"` 内，`llama_model` / `llama_context` 是不透明句柄 | ❌ **没有 C ABI**：发行版只有 `.exe`+DLL，无头文件/导入库；上游明写 headers 不作为 SDK 分发 |
| **形态** | **进程内静态链接**（`libllama.a` 8.5 MB 直接编进插件） | **进程监管 + OpenAI HTTP**（拉起 `ninfer-serve-*.exe`） |
| 服务名 | `zenith.ai.ILlamaInference` v1 | `zenith.ai.INinferInference` v1 |
| 实测 | 10.18 GB `Qwen3.8-27B-UD-IQ3_XXS.gguf`：加载 **1.2 s**（mmap），24 token 生成 **4.7 s**（CPU 8 线程） | `Ternary-Bonsai-2-27B`：TTFT **339 ms** / 预填 **3,350 tok/s** / 解码 **282 tok/s** / 投机接受 **88%** |
| 代价 | 换模型需重启进程；本机链接的是 CPU 后端（无 GPU 加速） | 每请求多一跳 loopback HTTP；引擎崩溃需重启 |

**为什么 NInfer 不能进程内**（三条取证）：① 本机 `infer-engine-sm120a` 包只有 `ninfer-serve-120a.exe` + 运行时 DLL，无 `llama.h` 式头文件、无 `.lib`/`.dll.a`；② 上游 `Neroued/ninfer` README 明写 *"The in-tree C++ headers are not distributed as an installed SDK"*，对外只有 CLI 与 HTTP；③ KVMem 分页、MTP/DFlash2、视觉驻留全在 server 进程内，公开 C 入口不存在。

⇒ `ai_ninfer` 的 `backend()` 直接返回 **`ninfer-http (no C ABI)`**，插件初始化时还会打一条 WARN 说明同一件事——不靠文档靠运行时可观测，避免运维误判它是库插件。详见 [plugins/ai_ninfer/README.md](plugins/ai_ninfer/README.md)。

**llama.cpp 从源码构建**（本机实测，MinGW + CPU 后端）：

```powershell
cmake -S C:\APP\GitHubCode\llama.cpp -B ..\llama-build-mingw -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF `
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF `
  -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_COMMON=OFF
cmake --build ..\llama-build-mingw --target llama
# 然后把 include/llama.h + ggml 头、libllama.a + ggml*.a 装到前缀，配置时传：
#   -DZENITH_LLAMA_ROOT=<前缀>
```

⚠️ ggml 默认开 OpenMP，静态库带 `GOMP_*` 引用——插件侧必须 `find_package(OpenMP)` 链接 `OpenMP::OpenMP_CXX`，否则链接失败（本仓已处理）。要 GPU 就改用 MSVC + CUDA 重编 llama.cpp，接口不变、插件源码一行不用改。

**NInfer 也能进程内——但 C ABI 得我们自己补**（`ai_ninfer_native`）：

| | `ai_llama` | `ai_ninfer_native` |
|---|---|---|
| C ABI 谁提供 | llama.cpp 自带 `llama.h` | **自研**：`zenith/ai/ninfer_c_api.h`（POD + opaque + 调用方缓冲 + C 回调） |
| 编译器 | MinGW（和宿主一致） | **MSVC + CUDA**（引擎只能这样编），宿主 MinGW 只碰纯 C |
| 产物 | 7.3 MB DLL | **550.8 MB DLL**（引擎 + CUDA kernel 全静态编入） |
| 实测 | 10.18 GB GGUF：加载 1.0 s，24 token 4.4 s（CPU） | Ternary-Bonsai-2-27B v3：加载 **4.7 s**，485 ms 出 39 字节，**投机接受 36/104** |

三条布局铁律：只用 C 类型/opaque 指针；缓冲由调用方给；内存同侧 new/delete。违反即崩溃。

构建与踩坑（4 个，全已修）见 [plugins/ai_ninfer_native/README.md](plugins/ai_ninfer_native/README.md)；可复现补丁脚本在 `plugins/ai_ninfer_native/patches/`；
完整实测数据（档位矩阵 / 思考开关 / 复现命令）见 [plugins/ai_ninfer_native/RESULTS.md](plugins/ai_ninfer_native/RESULTS.md)。

**配套桌面端**：`examples/07_ai_gateway` 把 AI 插件挂成 OpenAI 兼容网关
（`/v1/*` + `/zenith/ai/status` + `/metrics`），仓库外的 `../ZenithDesk`
（React 19 + Electron 45）可直接连它做流式对话与引擎状态面板。

**框架侧连带修复**：`DynLib::load` 在 Windows 改用 `LoadLibraryExA(..., LOAD_WITH_ALTERED_SEARCH_PATH)`——默认搜索顺序里没有"被加载 DLL 自己的目录"，导致插件自带的私有依赖 DLL 一律 126 失败。
### 插件热重载（M8.2）

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
| `ai_ninfer_native`（进程内） | ~3.5 s | 引擎析构 + 重新加载 8.9 GB 权重（显存会先释放再占用） |

回归：`test_plugin_reload`（3 例）——`.new` 交换后服务被重注册且可用；坏文件不崩宿主；
真 GGUF（`Qwen3.8-27B-IQ3_XXS`）卸载后重载仍能推理。

**配置文件变了也想让插件重新读配置**，不用重启宿主（三行）：

```cpp
config::HotReloader cfgFile("./zenith.json", 500);
cfgFile.onChange([&](auto) {                 // 内容哈希变化才触发
    for (const auto& p : mgr.plugins()) mgr.reload(p->path);   // 重新 init() → 重新读 config
});
cfgFile.start();
```

> AI 插件重载会连模型/引擎一起重建（见上表），配置这类"高频小改"更建议做成
> 每请求读取而非重载插件。
## M9：流式响应 / 压缩 / 非对称身份（✅ 已完成）

- **`http::SseStream` + `http::ChunkWriter`** —— HTTP/1.1 分块流式响应。
  `Context::sse()` 一行起流，事件帧（`event` / `id` / 多行 `data` / 心跳注释）由框架编码；
  写端线程安全（任意工作线程可推送），流式连接不参与 keep-alive 复用，
  空闲踢除自动让路。`ChunkWriter` 供大响应边算边传
- **`http::gzipMiddleware`** —— 响应压缩（zlib 后端，未探测到即降级 identity）：
  `Accept-Encoding` 协商（含 `q=0` 拒绝）、体积阈值、204/304/HEAD 自动跳过
- **`jwt::asym`** —— RS256/384/512、ES256/384 签发与校验（OpenSSL 后端）：
  ECDSA 按 JWS 要求输出定长 `R‖S`（非 DER），并显式拒绝 `none` / `HS*` 防算法混淆
- 新增回归测试：`Stream.*`（5）、`Compress.*`（4）、`JwtAsym.*`（5）
- 意义：`ai_inference` 的 token 流、`ai_mcp` 的 Streamable HTTP 响应流、
  A2A 的 `message/stream` 三者共用同一地基，已不再被框架缺口卡住

```cpp
// 一行起流：SSE 头由框架写，正文按 WHATWG EventSource 语义编码
server.router().get("/v1/chat/stream", [](http::Context& c) {
    auto sse = c.sse(200, {{"X-Model", "zenith-7b"}});           // 响应头此刻已发出
    sse->send(R"({"choices":[{"delta":{"content":"天顶"}}]})");   // data: <json>
    sse->send("1", "progress", "evt-1");                         // event + id
    sse->comment();                                              // 心跳，防反代超时
    sse->close();                                                // 0\r\n\r\n + 关连接
});
// 推流不必发生在 handler 内：写端线程安全，可在任意工作线程继续 send()
// 反例：流式响应不做整包压缩，也不复用 keep-alive（连接随流结束关闭）
```

## M5.7：SPA 托管 + 实战沉淀

- **`http::spaHandler(rootDir)`**：单页应用静态服务——真实文件优先
  （ETag/304/Range/小文件缓存），未命中的前端路由回 index.html（带
  `Cache-Control: no-cache`），带扩展名的 404 资源保持 404 不伪装
- **Windows 控制台 UTF-8**：ConsoleLogger 自动切换代码页，中文日志不再乱码
- **SDK 消费全流程验证**：`find_package(Zenith CONFIG)` + `Zenith::zenith_core`
  在 Windows/MinGW 独立链接外部应用（ORM=OFF 裁剪，不传递 uORM/DB 驱动依赖）
- **首个实战应用迁出**：技术博客 + 3D 联机棋室（React 19 + Three.js，
  `../ZenithBlog`），经 SDK 消费；踩坑 7 条沉淀入 `docs/ARCHITECTURE.md` §5g
- 新增回归测试：`SpaE2E.FallbackAndAssetPriority`、`Router.RootPathNotMatchedByCatchAll`

## M5：生产就绪

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

## 插件生态

### 已实装插件（9 个）

| 插件 | 功能 | 技术 |
|---|---|---|
| `hello_cpp` | C++ 虚接口服务示例 | 纯 C ABI |
| `hello_c` | **纯 C 编译器**构建的示例插件 | C89 兼容 |
| `media_ffmpeg` | probe / 截图 / 转码（进度回调） | FFmpeg SDK |
| `edge_esp32` | COBS 帧设备桥（TCP 网关 + 串口） | 自研协议 |
| `ai_comfyui` | ComfyUI workflow 提交 + 产物下载 | 自研 HTTP |
| `ai_inference` | 通用 LLM 网关：mock / 外部 OpenAI 上游 + SSE 流式 | 纯 C ABI 插件 |
| `ai_llama` | **进程内 llama.cpp**（真·C ABI 静态链接，可加载本地 GGUF） | `llama.h` C ABI |
| `ai_ninfer` | **NInfer 引擎**：进程监管 + OpenAI HTTP（引擎无 C ABI，见下文） | 子进程 + HTTP |
| `ai_ninfer_native` | **NInfer 进程内**：自研纯 C ABI shim（MSVC 侧编进 550 MB DLL）+ 宿主适配器 | 自研 C ABI |
| `db_vector` | **向量存储**：自研 HNSW（Malkov'16 全实现：启发式选邻/多层图/墓碑删除/持久化）+ Qdrant 兼容 API | 纯 C ABI 插件 |
| `ai_mcp` | **MCP 网关**（2026-07-28 基线）：JSON-RPC over HTTP，把向量库/推理自动桥接为 Agent 工具 | 纯 C ABI 插件 |
| `ai_agent` | **Agent 运行时**：ReAct 工具循环（JSON 协议+容错降级）、会话滚动记忆、RAG 召回注入、MCP 远端工具 | 纯 C ABI 插件 |

### 预计适配插件（M8 生态扩展，2026 最热方向）

| 插件 | 功能 | 为什么最火 | 优先级 |
|---|---|---|---|
| `net_mqtt` | MQTT 3.1.1/5.0 broker（QoS/共享订阅/保留消息） | IoT 标准消息协议 | ⭐⭐⭐⭐ |
| `net_grpc` | gRPC 框架（HTTP/2 + Protobuf + 四种流模式） | 微服务/AI 编排标准通信 | ⭐⭐⭐ |
| `media_webrtc` | WebRTC 实时通信（ICE/DTLS/SRTP + FFmpeg 转码联动） | 视频会议/AI 实时视频 | ⭐⭐⭐ |
| `obs_otel` | OpenTelemetry 分布式追踪（OTLP + W3C Trace Context） | 云原生可观测标准 | ⭐⭐⭐ |
| `ai_stt` | 语音识别（whisper.cpp + 实时流式 + VAD） | 语音 AI 入口 | ⭐⭐⭐ |

→ 完整方案见 [docs/PLAN_M8.md](docs/PLAN_M8.md)（功能/架构/代码量/依赖逐项标注）

## 里程碑

- **M1 ✅ 插件内核**：C ABI v1、服务注册表、插件管理、线程池、日志、双示例插件、uORM 集成
- **M2 ✅ Reactor + HTTP**：EventLoop（WSAPoll/epoll/kqueue/select 四后端）、TCP 全家、自研
  HTTP/1.1 解析器、路由/中间件/Context、HttpServer/HttpClient、自研 JSON、协程 co_await 接入
- **M3 ✅ 数据层与协议扩展**：WebSocket(RFC6455)、自研 Redis RESP 客户端、JSON 配置、
  uORM 服务化（连接池 + ServiceRegistry + 专用 DB 线程池）、SHA1/Base64
- **M4 ✅ 生态与边缘**：TLS/OpenSSL、FFmpeg 媒体、edge 设备桥、ComfyUI AI、URL/连接池
- **M5 ✅ 生产就绪**：multipart/静态文件/Session/优雅停机/wss、可观测(指标/Request-Id/status)、
  安全(SHA-256/JWT/CORS/限流)、zenith-bench、CI/安装导出/脚手架、热重载/多进程/管理台
- **M6 ✅ v0.1.0 发布**：Release 基准(181K RPS)/连接上限/slowloris 防线/soak 1.64 亿零失败/
  Linux epoll 闭环/双平台发行版
- **M9 ✅ 流式与身份**：分块/SSE 流式响应、gzip 响应压缩、非对称 JWT（RS/ES）
- **M7 📋 性能深挖**：Context 池化+arena、sendfile 零拷贝、deflate 压缩、io_uring/IOCP、HTTP/2
  → [docs/PLAN_M7.md](docs/PLAN_M7.md)
- **M8 🚧 生态扩展（1/8）**：✅ M8.1 LLM 推理网关——实装为 **4 个插件**
  （`ai_inference` 网关 / `ai_llama` 进程内 llama.cpp / `ai_ninfer` 子进程 /
  `ai_ninfer_native` 自研 C ABI 进程内），实测 prefill 3.65k · decode 729 tok/s；
  📋 M8.2–M8.8：MCP/向量DB/MQTT/gRPC/WebRTC/OpenTelemetry/STT
  → [docs/PLAN_M8.md](docs/PLAN_M8.md)

## 依赖原则

**边界只有一条：C ABI。** 自研优先不变，但三方准入从「点名白名单」改为「边界规则」——
**凡提供稳定 C ABI 的库，均可直接接入，不再逐个立项审批**。

- **能自研的继续自研**：HTTP/1.1、WebSocket、JSON、EventLoop/Poller、路由、日志、
  线程池/无锁队列、配置、Redis RESP、JWT、SHA-1/SHA-256、HMAC、Base64、deflate——
  这些自己写，不受本原则放宽影响。
- **其余只要能过 C ABI 就收**：`extern "C"` 符号 + 不透明句柄/自身结构体，头文件自洽，
  不要求调用方与被调方 C++ ABI 一致。这与插件内核是同一条边界——宿主只认 C ABI，
  跨编译器（MSVC/GCC/Clang）、跨 C++17/20 混编、跨二进制分发都不承担 C++ ABI 兼容义务。

接入只过四道闸，不设名录审批：

| 闸 | 要求 |
|---|---|
| 接口 | 纯 C ABI（`extern "C"` + 句柄/不透明指针），不把 STL/模板类型带过边界 |
| 许可 | MIT / BSD / Apache-2.0 / zlib / ISC 直接接入；GPL、AGPL 不进链接树，只能独立进程 + IPC 调用且需作者确认 |
| 可选 | 一律 `find_package` / `pkg-config` 探测 + 独立 `ZENITH_*` 开关；缺失即降级关闭，核心库照常链接 |
| 隔离 | 阻塞调用不得占 EventLoop——统一 `post()` 到专用线程池；不接管线程/事件循环模型 |

### 已接入 / 已排期的 C ABI 依赖

| 依赖 | 用途 | 形态 | 状态 |
|---|---|---|---|
| uORM | ORM 服务化（作者自有，MIT，submodule） | C++ 核心 + `uorm_c.h`（C ABI 目标默认关闭） | 已接入 |
| OpenSSL | TLS/DTLS、RSA/ECDSA 签名（JWS·ACME·Passkey·VAPID） | 纯 C | 已接入 |
| mbedTLS / wolfSSL | TLS 备选后端 | 纯 C | `ZENITH_TLS=` 已留位 |
| FFmpeg（libav*） | probe / 解码 / 转码 / 编码 | 纯 C | 已接入 |
| SQLite / libmariadb / libpq | uORM 驱动 | 纯 C | 已接入（自动探测） |
| zlib / libdeflate | deflate/gzip（已接入：`gzipMiddleware`；自研版可选留作练习） | 纯 C | 已接入 |
| llama.cpp（`llama.h`） | 引擎核心：纯 C ABI，静态链接进 `ai_llama` | 纯 C | 已接入 |
| ninfer-serve-*.exe | NInfer 引擎：**无 C ABI**，只能子进程 + OpenAI HTTP | 独立进程 | 已接入 |
| whisper.cpp（`whisper.h`） | STT 后端 | 纯 C | M8.8 |
| libsrtp | SRTP 媒体加密 | 纯 C | M8.6 |
| wasmtime-c-api | 沙箱插件宿主（语言无关 + 崩溃隔离） | 纯 C | M9 候选 |
| miniz / libarchive | ZIP 解包（DOCX/XLSX 读入、doc_parse） | 纯 C | M9 候选 |
| libmupdf | PDF 文本层 / 渲染 | 纯 C | M9 候选 |

> 选型口径：同一能力只留一个默认后端，其余走 `ZENITH_*` 开关——TLS 后端已经是这个模式。

### 仍不引入

| 情形 | 原因 |
|---|---|
| 纯 C++ 模板/头文件库，且无法薄封装成 C ABI | 会把 C++ ABI、STL 版本、编译器差异带进宿主 |
| 要求 C++ ABI 一致的预编译二进制（.lib/.a） | 与「跨编译器一致」的内核目标直接冲突 |
| 自带 reactor、要接管 fd 与线程的网络运行时 | 与自研 EventLoop 抢控制权 |

## 许可

Zenith 本体：MIT（待作者确认）。uORM：MIT（thirdparty/uORM）。
