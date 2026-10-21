# ai_ninfer_native —— **进程内** NInfer（自研 C ABI，MSVC 侧）

> 这是 `ai_ninfer`（子进程 + HTTP）的第二个形态：**引擎跑在宿主进程里**，
> 没有 loopback HTTP、没有子进程管理。代价是引擎崩溃＝宿主崩溃。
>
> 📊 **完整实测数据见 [RESULTS.md](RESULTS.md)**（档位矩阵 / 思考开关 / 复现命令 / 已知坑）。
>
> 与 `ai_llama` 的区别不在"能不能进程内"，而在**那一层 C ABI 是谁的**：
> llama.cpp 自带 `llama.h`（`extern "C"` + 不透明句柄），直接链；
> NInfer 只有 C++ 类 API，**这一层是我们自己补的**。

## 1. 为什么需要自研 C ABI

| | llama.cpp | NInfer |
|---|---|---|
| 公开接口 | `extern "C"` + `struct llama_model;` 不透明句柄 | `ninfer::Engine` 等 **C++ 类**（`std::string_view` / `std::vector` / `unique_ptr`） |
| 能否跨编译器边界 | 能（纯 C） | **不能**（C++ 对象跨 MSVC↔MinGW = UB） |
| 编译工具链 | CPU 后端可用 MinGW，和宿主一致 | CUDA 引擎**只能 MSVC** |

结论：想在 MinGW 宿主里用 MSVC 编的引擎，边界上**只允许**纯 C ——
所以本插件自建了三样东西：

```
zenith/ai/ninfer_c_api.h          纯 C 契约（POD / opaque / 调用方缓冲 / C 回调）
plugins/ai_ninfer_native/
  ├─ ninfer_c_shim.cpp            MSVC：把 ninfer::Engine 包成 5 个 extern "C" 函数
  └─ plugin_entry.cpp             MSVC：把 shim 装成 zenith_nz_service 表并注册
src/ai/ninfer_c_api_client.cpp    MinGW：把 C 表包成 IInferenceService
```

注册服务名 `zenith.ai.INinferCApi` v1；宿主侧用
`wrapNinferCAbiService(registry.queryRaw(...))` 拿到 `IInferenceService`，
之后 `mountOpenAiApi` 一视同仁（与 llama / mock 后端完全同构）。

## 2. 布局规则（违反即崩，不是建议）

1. 只用 C 基本类型 / POD 结构 / opaque 指针；
2. 字符串与数组由**调用方**提供缓冲（`zenith_nz_result::content[16384]`），
   本 API 绝不返回内部分配的指针；
3. 回调是 C 函数指针（Win64 只有一种调用约定）；
4. 内存分配与释放**同侧**（`nz_*` 内部 new/delete 配对）。

> 定长缓冲意味着超长回复会被截断：`zenith_nz_result::content_truncated` 会置 1。
> 需要无限长就只用流式回调（`zenith_nz_token_cb`）自行拼接。

## 3. 从源码构建（本机实测通过）

```powershell
# 前置：VS 2026 (MSVC 14.5x) + CUDA 13.3 + Ninja + vcpkg(ffmpeg/curl/hwloc)
powershell -ExecutionPolicy Bypass -File .\patches\apply-ninfer-all-msvc.ps1 `
    -Src C:\Users\26809\Desktop\Code\c++\_deps\ninfer-all-src

# 在 vcvars64 环境里：
cmake -S C:\Users\26809\Desktop\Code\c++\_deps\ninfer-all-src `
      -B C:\Users\26809\Desktop\Code\c++\_deps\ninfer-build -G Ninja `
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES=120a `
  -DNINFER_BUILD_APPS=OFF -DBUILD_TESTING=OFF `
  -DCMAKE_TOOLCHAIN_FILE=C:/Users/26809/Desktop/Code/c++/_deps/vcpkg/scripts/buildsystems/vcpkg.cmake `
  -DVCPKG_TARGET_TRIPLET=x64-windows -DVCPKG_MANIFEST_MODE=OFF
cmake --build C:\Users\26809\Desktop\Code\c++\_deps\ninfer-build --target zenith_ai_ninfer_native --parallel 8
```

产物：`build/m9/plugins-native/zenith_ai_ninfer_native.dll`（**约 550 MB**，引擎与
CUDA kernel 全部静态编入）。运行时还要把 **FFmpeg / cuBLAS** 的 DLL 放在同目录：

| DLL | 来源 |
|---|---|
| `avcodec-63 / avformat-63 / avutil-61 / swscale-10 / swresample-7 / z.dll` | `C:\...\Code\c++\_deps\vcpkg\installed\x64-windows\bin` |
| `cublas64_13.dll` / `cublasLt64_13.dll` | CUDA 运行时（本机取自 prism 包，CUDA 13.x 通用） |

> 框架侧改动：`DynLib::load` 在 Windows 改用 `LoadLibraryExA(..., LOAD_WITH_ALTERED_SEARCH_PATH)`，
> 否则插件目录里的私有依赖 DLL 一律找不到（默认只搜 exe 目录/系统目录/PATH）。

## 4. 踩过的坑（都已修，值得记）

| 现象 | 根因 | 修法 |
|---|---|---|
| `error C3861: "_cvtsh_ss": 找不到标识符` | MSVC 不声明该 GCC/Clang 标量 intrinsic | 换向量形式 `_mm_cvtss_f32(_mm_cvtph_ps(...))`（该仓库另一文件里已有同款写法） |
| `LoadLibrary 失败 err=126` | 依赖 DLL 在插件目录，但默认搜索路径不含"被加载 DLL 自己的目录" | `LOAD_WITH_ALTERED_SEARCH_PATH` |
| `kv_capacity is outside the usable range for max_context` | NInfer 约束 `kv_capacity ≥ max_context`，且页数在区间内 | 配置同时给出 `max_context`，默认让两者相等（8192） |
| `test_ai_llama` 在 ctest 里 SEGFAULT | 550 MB 的 MSVC/CUDA 插件被放进了通用插件目录，所有测试都会加载它 | 原生插件独立目录 `plugins-native/`，只有本测试加载 |

## 5. 实测（RTX 5070 Ti / Ternary-Bonsai-2-27B v3 / kv 8192 / MTP 4）

```
[ native ] 引擎+插件就绪 4711 ms, backend=ninfer-c-abi (in-process)
[ native ] models: {"id":"bonsai2-27b","backend":"ninfer-c-abi"}
[ native ] 485 ms，回复 39 字节：我是通义千问，一个乐于帮你
[ native ] 引擎计数：requests=1 draft=36/104      ← 投机解码接受 34.6%
```

对比同模型的子进程形态（`ai_ninfer`）：TTFT 339 ms / 解码 282 tok/s。
进程内省掉的是**一跳 loopback HTTP 和一个子进程**，不是算力。

## 5a. 投机解码档位 A/B（2026-10-21 实测，同一模型同一 prompt）

输入 1133 token（数数字语料）/ 输出 400 token / `temperature=0` / 关思考：

| 档位 | prefill tok/s | decode tok/s | 投机接受 | 墙钟 |
|---|---:|---:|---:|---:|
| MTP d1 | 3,558.8 | 170.7 | 199/200 (100%) | 2,665 ms |
| MTP d2 | 3,559.2 | 236.4 | 266/266 (100%) | 2,012 ms |
| **MTP d4（出厂默认）** | 3,551.6 | **314.3** | 314/344 (91%) | 1,593 ms |
| MTP d8 | 3,521.5 | 334.2 | 340/488 (70%) | 1,520 ms |
| DFlash2 d4 | 3,652.5 | 372.8 | 319/320 (100%) | 1,385 ms |
| DFlash2 d8 | 3,649.9 | 586.7 | 354/360 (98%) | 994 ms |
| DFlash2 d10 | 3,648.9 | 673.9 | 361/380 (95%) | 905 ms |
| **DFlash2 d12（最优）** | 3,667.7 | **729.0** | 365/408 (89%) | **859 ms** |
| DFlash2 d15 | 3,653.9 | 693.2 | 365/510 (72%) | 902 ms |

**结论：瓶颈是草稿质量/窗口，不是带宽也不是预填。**

- 预填恒定 3.5–3.7k tok/s，与 `spec` 无关 —— 说明 GPU 喂得动，卡不在读 prompt。
- 每次校验轮解出的 token 数：MTP d4 ≈ 4.7，DF2 d12 ≈ 11.8 —— 解码速度几乎与它成正比。
- 窗口继续加宽收益反转（DF2 d15 接受率掉到 72%，慢于 d12），存在明确最优点。
- 出厂默认 MTP d4 只跑到最优档的 **43%**；换 DF2 d12 后 decode **×2.32**，整条请求 **1.59 s → 0.86 s**。

配置（两个形态都适用）：

```jsonc
// 进程内插件
{ "ai.ninfer_native.spec": "3", "ai.ninfer_native.draft_tokens": "12" }
// 子进程插件：把引擎参数改成
//   --spec dflash2 --draft-tokens 12
```

> 前提：模型 artifact 要带 DFlash2 伴生权重（`Ternary-Bonsai-2-27B-ninfer-v3` 带）。
> 不带的模型回落到 MTP。

## 5b. 开启思考之后（同一模型，DFlash2 d12）

思考模式由 `ai.ninfer_native` 的 C ABI `enable_thinking`（`-1` 关 / `1` 开）控制，
映射到引擎的 `PromptOptions::enable_thinking`：

| 场景 | 输入 | prefill | decode | 投机接受 | 说明 |
|---|---:|---:|---:|---:|---|
| 思考关（1133 进 / 400 出） | 1,133 | 3,646 | **728.7** | 89% | 数数字语料 |
| 思考开 · 长思考（8192 出） | 1,173 | 3,650 | **419.2** | 50% | 输出打满 8192 仍未想完 |
| 思考开 · 短问答（97 进 / 351 出） | 97 | 1,394 | **308.6** | 32% | 正常推理题，1.2 s 出答案 |
| 思考开 · MTP d4（对照） | 1,173 | 3,575 | 185.3 | 44% | 同为思考模式，DF2 仍快 2.3× |

**结论**：思考本身会让推测解码失效一部分——接受率从 89% 掉到 30–50%，
decode 从 ~729 掉到 ~310–420（取决于思考内容的可预测性）。
DFlash2 在思考模式下依然是最优档位（2.3× MTP d4）。

两个由此暴露并修掉的问题：

1. **`reasoning` 缓冲 2 KB 太小**：真实思考文本 13 KB+，已抬到 16 KB（`content` 同为 16 KB）。
2. **默认 `max_context` 8 K 撑不住思考**：提示 1.2 K + 思考 8 K 会直接 `finish=context_capacity`，
   默认已抬到 **16 K**（思考 + 长回答场景建议给 32 K）。



## 6. 什么时候用哪个

| | `ai_ninfer`（子进程） | `ai_ninfer_native`（本插件） |
|---|---|---|
| 引擎崩溃 | 只死子进程，宿主可重启它 | **宿主一起死** |
| 延迟 | 多一跳 loopback HTTP（微秒级） | 无 |
| 部署 | 引擎 exe 与宿主解耦，可单独升级 | 换引擎要重编 550 MB DLL |
| 工具链 | 纯 MinGW，零 MSVC 依赖 | 需要 MSVC + CUDA + vcpkg |
| 适用 | 生产默认 | 延迟敏感 / 不想管子进程 |
