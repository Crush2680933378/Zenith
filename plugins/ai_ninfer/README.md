# ai_ninfer —— NInfer 引擎专用插件

> ## ⚠️ 结论先说：NInfer **没有 C ABI，无法进程内嵌入**
>
> 这个插件**不是**「链接一个库」的插件，而是「**拉起一个进程 + 说 OpenAI 协议**」的插件。
> 它跟 `ai_llama`（真·C ABI 静态链接）在形态上完全不同，别把两者混着理解。

## 取证（2026-10-21，本机实测）

| # | 证据 | 说明 |
|---|---|---|
| 1 | `C:\Users\26809\Desktop\infer-engine-sm120a-20261002\engine\` 只有 `ninfer-serve-120a.exe` + 运行时 DLL | **无 `llama.h` 之类头文件、无 `.lib`/`.dll.a` 导入库**，无从链接 |
| 2 | 上游 [Neroued/ninfer](https://github.com/Neroued/ninfer) README 明写 *"The in-tree C++ headers are not distributed as an installed SDK"* | 官方不发布 SDK；对外只有 CLI 与 OpenAI / Anthropic HTTP 接口 |
| 3 | 其核心能力（KVMem 分页、MTP/DFlash2 投机解码、视觉驻留）都在 server 进程内部 | 即使拿到头文件，也没有对应的公开 C 入口 |

对比 `llama.h`：`extern "C" {}` + 不透明 `struct llama_model;` / `struct llama_context;` +
`LLAMA_API` 导出控制 —— 那才是可嵌入的 C ABI。

## 所以本插件做什么

```
Zenith 宿主进程
  └── ai_ninfer 插件（本模块）
        ├── EngineSupervisor：CreateProcess/fork+exec、stdout/stderr 管道、
        │                     就绪标记探测（"listening on"）、崩溃重启限次、
        │                     Job 对象（KILL_ON_JOB_CLOSE，宿主崩了不留孤儿）
        └── OpenAiUpstreamBackend：HTTP 说 /v1/chat/completions（含 SSE 流式）
                                    + 解析 timings（prefill/decode/投机接受率）
                    ↓ loopback HTTP
        ninfer-serve-120a.exe（NInfer 引擎，能力全在这里）
```

- **代价**：每请求多一跳 loopback HTTP（≈几十微秒，相对 27B 模型的 prefill 可忽略）。
- **收益**：引擎原生能力（三元量化、KVMem、投机解码、视觉）一项不丢。
- **注册服务名**：`zenith.ai.INinferInference` v1 —— 与 `ai_llama`
  （`zenith.ai.ILlamaInference`）、`ai_inference`（`zenith.ai.IInferenceService`）
  **互不冲突**，可在同一进程共存。
- **可见性**：`backend()` 返回 `"ninfer-http (no C ABI)"`，插件初始化时还会打一条
  WARN 级日志说明同一件事，避免运维误判。

## 配置（对应本机 infer-engine-sm120a 包）

```json
{
  "ai.ninfer.exe":   "C:\\Users\\26809\\Desktop\\infer-engine-sm120a-20261002\\engine\\ninfer-serve-120a.exe",
  "ai.ninfer.model": "C:\\Users\\26809\\Desktop\\infer-engine-sm120a-20261002\\models\\Ternary-Bonsai-2-27B-ninfer-v3-mtponly.ninfer",
  "ai.ninfer.args":  "<MODEL> --host 127.0.0.1 --port 8111 --model-id qwen3.8-27b --max-context 32768 --kv-capacity 8192 --kv-dtype k8v4 --host-kv-mib 4096 --prefill-chunk 1024 --spec mtp --draft-tokens 4 --default-max-tokens 32768 --default-reasoning-effort none --max-concurrency 1 --max-shared-prefixes 0 --presence-penalty 0 --temperature 0.7 --top-p 0.9 --top-k 20",
  "ai.ninfer.env":   "NINFER_KV_WINDOW=16384;NINFER_KV_RETRIEVE=8192;NINFER_KV_RING=1;NINFER_HOST_PAGEABLE=1;NINFER_KV_REUSE_HOSTBACKED=1",
  "ai.ninfer.model_id": "qwen3.8-27b"
}
```

`<MODEL>` / `<ENGINE_DIR>` 由插件展开；不填 `ai.ninfer.url` 时自动从 `--host/--port` 推断。

## 何时能改成进程内

上游发布 **C ABI 头文件 + 导入库**（哪怕只是 `ninfer_create/chat/destroy` 三个函数），
本插件就能退化成 `ai_llama` 那种静态链接形态。在那之前，进程边界不可绕过。
