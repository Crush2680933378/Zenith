# LLM 推理网关（`ai_inference`）

> 本文是 README 的延伸：推理网关的形态、引擎监管、以及对「为什么不做进程内 llama.cpp」的取证。
> 快速上手见 [README 快速开始](../README.md#快速开始)。

## 三种形态

`ai_inference` 插件（纯 C ABI，服务 `zenith.ai.IInferenceService` v1）：

| 形态 | 做什么 |
|---|---|
| `engine` | **拉起并监管**本地引擎进程（NInfer / llama-server…），等日志就绪后转 OpenAI 协议对接；插件卸载时回收进程 |
| `openai` | 接已在跑的外部 OpenAI 兼容服务（vLLM / 云端 API / 你手动起的引擎） |
| `mock` | 确定性后端：无模型也能跑通全部契约（CI 用的就是它） |

## EngineSupervisor

- `CreateProcess` / `fork+exec` + stdout/stderr 管道 + 就绪标记探测
- 崩溃重启（限次）+ Job 对象（Windows `KILL_ON_JOB_CLOSE`，宿主退出不留孤儿占显存）
- **解析引擎控制台统计行**（`TTFT / prefill / decode / mtp accepted`）

## OpenAI 兼容面

`zenith::ai::mountOpenAiApi(router, service, options)` —— 协议面在核心库，插件只管推理。

- `GET /v1/models`、`POST /v1/chat/completions`（`stream=true` 走 SSE）、
  `POST /v1/completions`、`POST /v1/embeddings`、`GET /zenith/ai/status`
- 参数域收敛、OpenAI 形状错误体、`reasoning_content` 分离透出（思考模型）
- **推理绝不占 EventLoop**：给 `options.pool` 就投递线程池；非流式用 chunked 延迟响应

```cpp
// 宿主侧装配：加载插件 → 取服务 → 挂 OpenAI 路由（三行）
plugin::PluginManager mgr(registry, &pool, &cfg);
(void)mgr.loadDirectory("./plugins");
auto* svc = static_cast<ai::IInferenceService*>(
    registry.queryRaw(ai::IInferenceService::service_name(), 1));
ai::mountOpenAiApi(server.router(), *svc, ai::OpenAiApiOptions{"/v1", &pool});
server.listen("0.0.0.0", 8080);   // 之后 curl / OpenAI SDK 直接指过来
```

## 引擎遥测进 Prometheus

`zenith_ai_decode_tps`、`zenith_ai_prefill_tps`、`zenith_ai_ttft_ms`、
`zenith_ai_draft_tokens_total`、`zenith_ai_draft_accepted_total`（投机解码接受率）、
`zenith_ai_engine_running`、`zenith_ai_cached_prompt_tokens_total`。

回归测试：`test_ai_inference`（11 例：契约 9 例 + 上游/引擎两个活体用例，未设环境变量自动跳过）。

## 为什么不做进程内 llama.cpp

| 取证 | 结论 |
|---|---|
| `llama.cpp-prism-b10709` 与 `infer-engine-sm120a` 都是**纯二进制发行版** | 没有 `llama.h`、没有 `.lib`/`.dll.a`，无法编译链接 |
| 源码树 `C:\APP\GitHubCode\llama.cpp` 是 **b10603**，二进制是 **b10709** | 跨 106 个 build，`llama_context_params` 这类按值返回的结构体布局无保证 |
| `--kv-mean-center` / `--spec mtp\|dflash2` / KVMem 分页 | 全在 **server 层**，公开 C API 里没有对应入口 |

结论：引擎的能力在进程内、只认自己的目录，硬链接既拿不到头文件也拿不到这些特性。
正确形态是「宿主拉进程 + 说 OpenAI 协议」，插件负责生命周期与遥测。

## 对接 NInfer 引擎（`infer-engine-sm120a-20261002`）

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

## 三个引擎插件的 C ABI 分野

| | `ai_llama` | `ai_ninfer` | `ai_ninfer_native` |
|---|---|---|---|
| **C ABI** | ✅ `llama.h` 全在 `extern "C"` 内，不透明句柄 | ❌ 发行版只有 `.exe`+DLL | **自研** `zenith/ai/ninfer_c_api.h` |
| **形态** | 进程内静态链接（`libllama.a` 8.5 MB） | 进程监管 + OpenAI HTTP | MSVC 侧编进 550.8 MB DLL |
| 服务名 | `zenith.ai.ILlamaInference` v1 | `zenith.ai.INinferInference` v1 | 同接口 |
| 实测 | 10.18 GB GGUF：加载 1.2 s（mmap），24 token 4.7 s（CPU 8 线程） | TTFT 339 ms / 预填 3,350 tok/s / 解码 282 tok/s / 投机接受 88% | 加载 4.7 s，485 ms 出 39 字节，投机接受 36/104 |
| 代价 | 换模型需重启进程；CPU 后端 | 每请求多一跳 loopback HTTP；引擎崩溃需重启 | 编译器只能 MSVC+CUDA；DLL 巨大 |

**为什么 NInfer 不能进程内**（三条取证）：① 包内只有 `ninfer-serve-120a.exe` + 运行时 DLL，
无头文件、无 `.lib`/`.dll.a`；② 上游 README 明写 *"The in-tree C++ headers are not distributed
as an installed SDK"*；③ KVMem 分页、MTP/DFlash2、视觉驻留全在 server 进程内。

⇒ `ai_ninfer` 的 `backend()` 直接返回 **`ninfer-http (no C ABI)`**，插件初始化时还会打一条
WARN——不靠文档靠运行时可观测。详见 [plugins/ai_ninfer/README.md](../plugins/ai_ninfer/README.md)。

三条布局铁律（`ai_ninfer_native`）：只用 C 类型/opaque 指针；缓冲由调用方给；内存同侧
new/delete。违反即崩溃。构建与踩坑见
[plugins/ai_ninfer_native/README.md](../plugins/ai_ninfer_native/README.md)，
实测矩阵见 [RESULTS.md](../plugins/ai_ninfer_native/RESULTS.md)。

## 从源码构建 llama.cpp（MinGW + CPU）

```powershell
cmake -S C:\APP\GitHubCode\llama.cpp -B ..\llama-build-mingw -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DGGML_CUDA=OFF -DLLAMA_CURL=OFF -DBUILD_SHARED_LIBS=OFF `
  -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_SERVER=OFF `
  -DLLAMA_BUILD_TOOLS=OFF -DLLAMA_BUILD_COMMON=OFF
cmake --build ..\llama-build-mingw --target llama

# 然后把 include/llama.h + ggml 头、libllama.a + ggml*.a 装到前缀，配置时传：
#   -DZENITH_LLAMA_ROOT=<前缀>
```

> ⚠️ ggml 默认开 OpenMP，静态库带 `GOMP_*` 引用——插件侧必须 `find_package(OpenMP)`
> 链接 `OpenMP::OpenMP_CXX`，否则链接失败（本仓已处理）。要 GPU 就改用 MSVC + CUDA
> 重编 llama.cpp，接口不变、插件源码一行不用改。

**框架侧连带修复**：`DynLib::load` 在 Windows 改用
`LoadLibraryExA(..., LOAD_WITH_ALTERED_SEARCH_PATH)`——默认搜索顺序里没有"被加载 DLL
自己的目录"，导致插件自带的私有依赖 DLL 一律 126 失败。

## 配套桌面端

`examples/07_ai_gateway` 把 AI 插件挂成 OpenAI 兼容网关（`/v1/*` + `/zenith/ai/status` +
`/metrics`），仓库外的 `../ZenithDesk`（React 19 + Electron 45）可直接连它做流式对话与
引擎状态面板。
