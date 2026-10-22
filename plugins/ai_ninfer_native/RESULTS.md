# ai_ninfer_native 实测记录

> 一次完整测量的原始数据，用于回归对比。测量日期 **2026-10-21**。
> 换显卡 / 换模型 / 换引擎版本后重跑 `test_ai_ninfer_native` 即可重新生成对照。

## 环境

| 项 | 值 |
|---|---|
| GPU | NVIDIA GeForce RTX 5070 Ti 16 GB（sm_120a） |
| 驱动 / CUDA | 616.92 / CUDA 13.3（nvcc 13.3.73） |
| 引擎源码 | `iamwavecut/ninfer-all`（Windows 分支）+ 本仓补丁 `patches/apply-ninfer-all-msvc.ps1` |
| 编译 | MSVC 14.51（VS BuildTools 18）+ Ninja + vcpkg（`x64-windows`） |
| 模型 | `Ternary-Bonsai-2-27B-ninfer-v3.ninfer`（8.87 GB，自带 MTP + DFlash2 伴生权重） |
| 插件 | `zenith_ai_ninfer_native.dll` 550.8 MB（引擎与 CUDA kernel 静态编入） |
| 形态 | **进程内**（无子进程、无 HTTP） |
| 语料 | 该包教程 §0.2 的「数数字」：`1 2 … 300` + 续写指令 = **1,133 token**（思考关）/ 1,173（思考开，多一轮模板） |
| 采样 | `temperature=0`，`max_tokens` 见各表 |

## 1. 投机解码档位矩阵（思考关，1,133 进 / 400 出）

| 档位 | prefill tok/s | decode tok/s | 投机接受 | 墙钟 |
|---|---:|---:|---:|---:|
| MTP d1 | 3,558.8 | 170.7 | 199/200 (100%) | 2,665 ms |
| MTP d2 | 3,559.2 | 236.4 | 266/266 (100%) | 2,012 ms |
| MTP d4（出厂默认） | 3,551.6 | 314.3 | 314/344 (91%) | 1,593 ms |
| MTP d8 | 3,521.5 | 334.2 | 340/488 (70%) | 1,520 ms |
| DFlash2 d4 | 3,652.5 | 372.8 | 319/320 (100%) | 1,385 ms |
| DFlash2 d8 | 3,649.9 | 586.7 | 354/360 (98%) | 994 ms |
| DFlash2 d10 | 3,648.9 | 673.9 | 361/380 (95%) | 905 ms |
| **DFlash2 d12（最优）** | 3,667.7 | **729.0** | 365/408 (89%) | **859 ms** |
| DFlash2 d15 | 3,653.9 | 693.2 | 365/510 (72%) | 902 ms |

**读法**：预填恒定 3.5–3.7k tok/s（与档位无关）⇒ 瓶颈不在读 prompt、也不在显存带宽；
解码速度 ≈ 每轮校验解出的 token 数（MTP d4 ≈ 4.7，DF2 d12 ≈ 11.8）。
窗口再宽收益反转（d15 接受率 72%），**12 是拐点**。

## 2. 思考开关的影响（DFlash2 d12）

思考由 C ABI 的 `enable_thinking`（`-1` 关 / `1` 开）控制，映射到引擎
`PromptOptions::enable_thinking`。

| 场景 | 输入 | 输出 | prefill | decode | 投机接受 | 说明 |
|---|---:|---:|---:|---:|---:|---|
| 思考关 | 1,133 | 400 | 3,646 | **728.7** | 89% | 数数字语料，1.6 s 内完成 |
| 思考开 · 长思考 | 1,173 | 8,192 | 3,650 | **419.2** | 50% | 打满 8192 仍未想完（`finish=length`），正文 0 字节 |
| 思考开 · 短问答 | 97 | 351 | 1,394 | **308.6** | 32% | 正常推理题（注水问题），1.2 s 出答案，正文 553 B + 思考 321 B |
| 思考开 · MTP d4（对照） | 1,173 | 2,000 | 3,575 | 185.3 | 44% | 同为思考模式，DFlash2 仍快 2.3× |

**读法**：
- 思考文本不可预测 ⇒ 接受率 89% → **30–50%**，decode 729 → **310–420**；
- 短 prompt 的 prefill 会被固定开销拉低（97 token 时只有 1,394 tok/s）；
- **DFlash2 在思考模式下依然是最优档位**（vs MTP d4 的 185）；
- 「数数字」这类 prompt 会让思考模型陷入长思考：8,192 预算都不够，10 s + 仍无正文。

## 3. 原始输出摘录

```
# 思考关 + DF2 d12
[ measure ] ok=1 墙钟=861 ms 正文=399 字节 思考=0 字节 finish=stop
[ measure ] 输入 1133 token / 输出 400 token
[ measure ] 输入(prefill) 3646.3 tok/s · 输出(decode) 728.7 tok/s · 投机 314/344
[ measure ] 前 60 字符：301 302 303 304 305 306 307 308 309 310 311 ...

# 思考开 + DF2 d12（8192 预算，长思考）
[ measure ] ok=1 墙钟=19882 ms 正文=0 字节 思考=13378 字节 finish=length
[ measure ] 输入 1173 token / 输出 8192 token
[ measure ] 输入(prefill) 3650.3 tok/s · 输出(decode) 419.2 tok/s · 投机 7015/14108

# 思考开 + DF2 d12（正常推理题，2048 预算）
[ measure ] ok=1 墙钟=1209 ms 正文=553 字节 思考=321 字节 finish=stop
[ measure ] 输入 97 token / 输出 351 token
[ measure ] 输入(prefill) 1394.5 tok/s · 输出(decode) 308.6 tok/s · 投机 278/876
[ measure ] 前 60 字符：设水池容量为 1。
```

## 3a. ABI v2 逐字流式验证（2026-10-22）

同一 prompt（"帮我写一个坦克世界大战的网页游戏使用three.js"）、`--thinking 1`、
`max_tokens=300`，直接看网关 SSE：

| 版本 | content 帧 | reasoning 帧 | 客户端观感 |
|---|---:|---:|---|
| v1（原） | 0 | 0 | 完全空白，只有收尾帧 |
| v1 + 聚合补发 | 0 | 1（整段 1163 字） | 结束后一次性出现思考 |
| **v2（本机重编后）** | 0 | **126** | **逐字实时出现** |

前 5 个 reasoning 片段（v2）：

```
"The"  " user is asking for a tank"  " World"  " War"  " game ("
```

> 为什么 v1 拿不到：`zenith_nz_token_cb(piece, len, user)` 没有 channel，
> shim 只转发 `OutputChannel::Content`；思考内容只存在于 `generate()` 的聚合结果里。
> v2 起回调带 `channel`（CONTENT=0 / REASONING=1），两路都实时回调。
> 宿主同时兼容 v1（思考退化为聚合结果）与 v2。

## 4. 复现

```powershell
# 默认（MTP d4，与出厂一致）
build\m9\bin\test_ai_ninfer_native.exe

# 指定档位 / 上下文 / 思考 / 预算
$env:ZENITH_TEST_NINFER_NATIVE_SPEC   = "3"      # 0=off 1=MTP 2=DFlash 3=DFlash2
$env:ZENITH_TEST_NINFER_NATIVE_DRAFT  = "12"
$env:ZENITH_TEST_NINFER_NATIVE_CTX    = "16384"  # max_context
$env:ZENITH_TEST_NINFER_NATIVE_KV     = "16384"  # kv_capacity（须 >= max_context）
$env:ZENITH_TEST_NINFER_NATIVE_THINK  = "1"      # -1 关 / 1 开
$env:ZENITH_TEST_NINFER_NATIVE_MAXTOK = "2048"
# 非 ASCII prompt 必须走文件（环境变量在 MinGW 下会变成 GBK）
$env:ZENITH_TEST_NINFER_NATIVE_PROMPT_FILE = "$env:TEMP\prompt-zh.txt"
build\m9\bin\test_ai_ninfer_native.exe
```

## 5. 复现时的已知坑

| 现象 | 原因 | 处理 |
|---|---|---|
| `finish=context_capacity` | 提示 + 思考超过 `max_context` | 思考场景给 32 K |
| 正文 0 字节、`finish=length` | 预算被思考吃光 | `max_tokens ≥ 8192`，或关思考 |
| `artifact:chat_template.jinja:`（消息为空） | 中文 prompt 经 `setenv` 传给 MinGW 进程被转 GBK | 走 UTF-8 文件，或用宽字符 API |
| 思考文本被截断在 2 KB | 早期 `reasoning` 缓冲太小 | 已改 16 KB；`content` 同为 16 KB |
| `kv_capacity is outside the usable range` | `kv_capacity` 必须 ≥ `max_context` | 两个一起给，默认都 16 K |

## 6. 这些数字不能说明什么

- **只有一张卡**（RTX 5070 Ti 16 GB），不能外推到 5090 / 4090；
- **短回复**（400 token）测的是稳态附近，长文档 prefill 会更慢；
- 接受率**高度依赖内容可预测性**：数数字 89%，正常推理 32%，真实业务文本落在两者之间；
- 未测：并发多请求、视觉输入、SSD KV 分层、pause/resume。
