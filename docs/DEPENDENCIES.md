# 依赖原则

**边界只有一条：C ABI。** 自研优先不变，但三方准入从「点名白名单」改为「边界规则」——
**凡提供稳定 C ABI 的库，均可直接接入，不再逐个立项审批**。

- **能自研的继续自研**：HTTP/1.1、WebSocket、JSON、EventLoop/Poller、路由、日志、
  线程池/无锁队列、配置、Redis RESP、JWT、SHA-1/SHA-256、HMAC、Base64、deflate。
- **其余只要能过 C ABI 就收**：`extern "C"` 符号 + 不透明句柄/自身结构体，头文件自洽，
  不要求调用方与被调方 C++ ABI 一致。这与插件内核是同一条边界——宿主只认 C ABI，
  跨编译器（MSVC/GCC/Clang）、跨 C++17/20 混编、跨二进制分发都不承担 C++ ABI 兼容义务。

## 四道闸

| 闸 | 要求 |
|---|---|
| 接口 | 纯 C ABI（`extern "C"` + 句柄/不透明指针），不把 STL/模板类型带过边界 |
| 许可 | MIT / BSD / Apache-2.0 / zlib / ISC 直接接入；GPL、AGPL 不进链接树，只能独立进程 + IPC 调用且需作者确认 |
| 可选 | 一律 `find_package` / `pkg-config` 探测 + 独立 `ZENITH_*` 开关；缺失即降级关闭，核心库照常链接 |
| 隔离 | 阻塞调用不得占 EventLoop——统一 `post()` 到专用线程池；不接管线程/事件循环模型 |

> 选型口径：同一能力只留一个默认后端，其余走 `ZENITH_*` 开关——TLS 后端已经是这个模式。

## 已接入 / 已排期

| 依赖 | 用途 | 形态 | 状态 |
|---|---|---|---|
| uORM | ORM 服务化（作者自有，MIT，submodule） | C++ 核心 + `uorm_c.h` | 已接入 |
| OpenSSL | TLS/DTLS、RSA/ECDSA 签名 | 纯 C | 已接入 |
| mbedTLS / wolfSSL | TLS 备选后端 | 纯 C | `ZENITH_TLS=` 已留位 |
| FFmpeg（libav*） | probe / 解码 / 转码 / 编码 | 纯 C | 已接入 |
| SQLite / libmariadb / libpq | uORM 驱动 | 纯 C | 已接入（自动探测） |
| zlib / libdeflate | deflate/gzip | 纯 C | 已接入 |
| llama.cpp（`llama.h`） | 引擎核心，静态链接进 `ai_llama` | 纯 C | 已接入 |
| ninfer-serve-*.exe | NInfer 引擎（无 C ABI） | 独立进程 | 已接入 |
| whisper.cpp（`whisper.h`） | STT 后端 | 纯 C | 已接入（可选） |
| libsrtp | SRTP 媒体加密 | 纯 C | M8.6 计划 |
| wasmtime-c-api | 沙箱插件宿主 | 纯 C | 候选 |
| miniz / libarchive | ZIP 解包（DOCX/XLSX） | 纯 C | 候选 |
| libmupdf | PDF 文本层 / 渲染 | 纯 C | 候选 |

## 仍不引入

| 情形 | 原因 |
|---|---|
| 纯 C++ 模板/头文件库，且无法薄封装成 C ABI | 会把 C++ ABI、STL 版本、编译器差异带进宿主 |
| 要求 C++ ABI 一致的预编译二进制（.lib/.a） | 与「跨编译器一致」的内核目标直接冲突 |
| 自带 reactor、要接管 fd 与线程的网络运行时 | 与自研 EventLoop 抢控制权 |
