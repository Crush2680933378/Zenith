# Zenith M4 计划 —— 生态与边缘（TLS / 媒体 / AI 绘画 / ESP32 / 泰山派）

> 前置状态：M1 插件内核 ✅ · M2 Reactor+HTTP ✅ · M3 WebSocket/Redis/配置/DB ✅
> 环境盘点（2026-10）：FFmpeg N-124739 完整 SDK（含 libav* 头）已在 PATH；
> MSYS2 OpenSSL 3.6.3（头 + 导入库 + CLI）就绪；ESP-IDF v5.5.4 在位；
> RK 交叉工具链待装（apt/发行版 aarch64-linux-gnu-g++）。

## 总原则

M4 的所有扩展能力一律走 **C ABI 插件**形态落地（media / ai / edge 三个插件 + plugins/ 目录），
宿主核心零改动即可获得新能力——这是对 M1 插件内核的最终实战检验。
三方白名单新增实际使用者：OpenSSL（TLS）、FFmpeg（媒体）。

---

## M4.1 TLS 实装（OpenSSL 后端）→ HTTPS

**交付物**
- `src/tls/openssl_backend.cpp`：`ITlsContext`/`ITlsSession`（M1 契约）首个后端；
  非阻塞握手状态机（WantRead/WantWrite 映射到 EventLoop 读/写兴趣切换），
  SNI、证书校验开关、ALPN（http/1.1）。
- `TlsStreamConnection`：把 ITlsSession 适配成「加解密层」，套在 TcpConnection 之上，
  对 HttpServer/HttpClient 透明。
- `HttpsServer` / `HttpClient` 的 https 支持：`ClientRequest.tls=true`。
- 测试：`openssl req` 现场生成自签证书 → HTTPS echo 端到端（curl --cacert 实测 +
  Zenith 客户端互测）；证书过期/主机不匹配 → 握手失败路径。
- CMake：`ZENITH_TLS=openssl` 时链接 libssl/libcrypto；none 语义不变。

**验收**：curl 以自签 CA 访问 Zenith HTTPS 服务器成功；https:// 外网地址 fetch 成功；
`ZENITH_TLS=none` 构建回归全绿。

**风险**：OpenSSL 与 Reactor 的非阻塞集成是经典难点（BIO 直写内存缓冲）；预留 2 天调试。

## M4.2 HTTP 客户端连接池 + URL（M3 遗留，M4.4 前置）

**交付物**
- `HttpClient` keep-alive 连接池：host:port → 空闲连接 LRU；`Connection: keep-alive` 复用；
  断连惰性剔除。
- URL 解析：`ClientRequest.url = "https://host:port/path?query"`（自研解析，入 http_common）。
- 压测脚本化：`tests/bench_http.cpp`（可选编）—— QPS/延迟粗测，验证池收益。

**验收**：连续 100 次请求同宿主 ≤ 3 个连接（keep-alive 生效）；URL 各形态单测。

## M4.3 media-ffmpeg 插件（C ABI 插件形态）

**交付物**
- `plugins/media_ffmpeg/`：C ABI 插件，注册 `IMediaService`（C++ 虚接口，同工具链）+
  纯 C vtable 形态。
- 能力集（首版）：
  - `probe(path) → 时长/编码/分辨率/流信息`（libavformat）
  - `snapshot(input, timeSec, outputJpg, w, h)`（解码 1 帧 → sws 缩放 → mjpeg 编码）
  - `transcode(input, output, {vcodec, acodec, crf, scale, faststart})`（进程内管线，
    libavformat/libavcodec/libswresample）
  - 事件回调：进度（timeus/total）→ 走宿主日志/服务回调
- FFmpeg 动态加载两种方式：编译期链接 `ZENITH_FFMPEG_ROOT`（默认探测 PATH 的
  ffmpeg-master-latest-win64-gpl-shared）。
- 测试：生成合成视频（ffmpeg testsrc）→ probe/snapshot/transcode 断言产物存在且可再 probe。
- 示例：`04_media`——HTTP 上传转码服务（multipart 解析顺手实现或先二进制 body）。

**验收**：插件被宿主加载并提供服务；transcode 产物 ffprobe 校验；FFmpeg 不在时插件优雅缺席。

## M4.4 ai-comfyui 插件（AI 绘画）

**交付物**
- `plugins/ai_comfyui/`：注册 `IAiDrawService`。
- 基于 M2 HttpClient + M3 WebSocket（全部自研栈）对接 ComfyUI：
  - `POST /prompt` 提交 workflow JSON（api 格式）→ prompt_id
  - `WS /ws?clientId=` 订阅 progress/executing/executed 事件
  - `GET /history/{prompt_id}` + `GET /view` 取回产物图片
  - prompt 占位符注入（seed 随机化、文本替换）
- 异步双形态：回调 + `co_await`（callback_await 复用）。
- 测试：本地无 ComfyUI 时用「Zenith 自建 mock ComfyUI」（复用我们自己的 HttpServer/WS ——
  又一次吃自家狗粮）跑通协议链；真实 ComfyUI 联机验收由用户执行。
- 示例：`05_ai_draw`——REST 提交任务 → WebSocket 推送进度 → 下载成图。

**验收**：mock 全链路绿；真实 ComfyUI 出图（用户验收）；AI 视频（AnimateDiff 等）走同一
workflow 通道，无额外开发。

## M4.5 edge-esp32 插件（设备桥）

**交付物**
- `zenith::io::SerialPort`（自研，win32 CreateFile/ReadFile + POSIX termios 双后端，
  列举/波特率/超时；EventLoop fd 适配——Windows 串口不能进 WSAPoll，用专用读线程 +
  runInLoop 汇入）。
- `plugins/edge_esp32/`：
  - 设备注册表（id → 串口/TCP 通道），`IDeviceBridge` 服务
  - 帧协议：COBS + CRC32（自研）+ JSON 载荷（心跳/属性/RPC）
  - REST/WS 桥：`/devices`、`/devices/:id/rpc`，设备事件 → WebSocket 广播
- ESP32 侧最小固件样例（ESP-IDF C，~200 行：WiFi + TCP 连 Zenith + COBS JSON 心跳/LED RPC）。
- 测试：无真机时用「伪串口/回环 TCP」mock 设备联调协议栈；真机验收由用户执行。

**验收**：mock 设备注册/心跳/RPC/事件广播全链路绿；ESP32 真机点亮 LED（用户验收）。

## M4.6 板卡与交叉编译落地

**交付物**
- RK3566/RK3576（aarch64 Linux）：装 `aarch64-linux-gnu-gcc/g++` 工具链 →
  用 `cmake/toolchains/aarch64-linux-gnu.cmake` 全量交叉构建 + 在板跑
  test_core/net/http(e2e 局部) —— 剩余依赖（无 WSAPoll，走 epoll 后端）为 epoll 首次实测。
- ESP-IDF component 化：`ports/esp-idf/zenith/`——core+log+concurrency 编为 IDF 组件；
  插件系统以「静态注册表」替代 dlopen（同一 ABI，宿主实现切换），验证 C ABI 在
  xtensa 上的真实可移植性。
- mbedTLS 后端（可选，若板卡场景需要轻量 TLS）。

**验收**：RK 板上 HTTP 服务可被局域网访问；ESP-IDF 组件编译并在 ESP32 跑通 echo 服务 +
静态插件注册。

## M4.7（可选）IOCP Proactor 后端

Windows 高并发终极形态；接口与 Reactor 不同，以独立 `IProactorTransport` 接入，
不撼动现有四后端。排期富余才做。

---

## 顺序与依赖

```
M4.1 TLS ──┐
M4.2 连接池 ─┼─→ M4.4 ai-comfyui（用 4.1/4.2 + M3 WS）
           └─→ M4.3 media-ffmpeg（独立，可并行）
M4.5 edge-esp32（独立，可与 4.3 并行）
M4.6 板卡（依赖 4.5 的固件样例收尾）
```

建议实施顺序：**4.1 → 4.2 → 4.3 ∥ 4.5 → 4.4 → 4.6 →（4.7 可选）**。
每个子阶段独立提交、独立验收，全部沿用 ZTEST + 端到端 + 压测的既有标准。
