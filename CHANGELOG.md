# Changelog

## Unreleased

### 新增
- **插件热重载（M8.2）**：`PluginManager::unload(name)` / `reload(path)` 支持单插件
  卸载与重载（安全序：stop → removeByOwner → shutdown → dlclose）；新增
  `PluginWatcher`——轮询插件目录，检测 `<name>.dll.new` 后执行
  「卸载旧插件 → 旧文件挪开 → `.new` 改名就位 → 重新 load+init+start」，失败回滚且宿主存活。
  Windows 下运行中的 DLL 不可覆盖，故 `.new` 交换是唯一可靠姿势。
  改配置也走 `reload()`（重新 `init()` 即重新读 config）。
  注意：重载后旧服务指针失效，消费者需重新 `queryRaw`。
  新增回归 `test_plugin_reload`（2 例：交换后服务重注册且可用；坏文件不崩宿主）。
- **`ai_ninfer_native` 插件（NInfer **进程内**，自研纯 C ABI）**：NInfer 只提供 C++ 类
  API 且 CUDA 引擎只能 MSVC 编译，而宿主是 MinGW——两边唯一安全交集是纯 C。于是自建四件套：
  `include/zenith/ai/ninfer_c_api.h`（POD + opaque + 调用方缓冲 + C 回调）、
  `plugins/ai_ninfer_native/ninfer_c_shim.cpp`（MSVC 侧把 `ninfer::Engine` 包成 extern "C"）、
  `plugin_entry.cpp`（装成 `zenith_nz_service` 表并以 `zenith.ai.INinferCApi` 注册）、
  `src/ai/ninfer_c_api_client.cpp`（MinGW 侧包成 `IInferenceService`）。
  实测 Ternary-Bonsai-2-27B v3：引擎加载 4.7 s、485 ms 出 39 字节、投机接受 36/104；
  产物 550.8 MB（引擎 + CUDA kernel 静态编入）。构建脚本与四处踩坑见
  `plugins/ai_ninfer_native/README.md` + `patches/apply-ninfer-all-msvc.ps1`。
- **框架修复 `DynLib`**：Windows 改用 `LoadLibraryExA(..., LOAD_WITH_ALTERED_SEARCH_PATH)`。
  默认搜索顺序不含"被加载 DLL 自己的目录"，插件把私有依赖（CUDA/FFmpeg 运行时）放在
  自己旁边时会一律 `LoadLibrary` 失败（err=126）。
- 新增测试 `test_ai_ninfer_native`（进程内引擎活体；缺模型自动 SKIP）；原生插件独立输出到
  `build/m9/plugins-native/`，不与示例插件混放（否则每个测试都会加载 550 MB 的 MSVC 模块）。
- `ai_llama` 插件（进程内 llama.cpp，纯 C ABI）：`llama.h` 全部在 `extern "C"` 内、
  `llama_model`/`llama_context` 为不透明句柄 ⇒ 可直接静态链接，无需子进程。
  llama.cpp 由本机源码（b10603）用 MinGW 编成 `libllama.a` 并装到
  `ZENITH_LLAMA_ROOT`；插件实现 `zenith.ai.ILlamaInference` v1，含 chat 模板应用、
  分词、KV 清空、分块预填、采样链（greedy/top-k/top-p/temp/dist）、逐 token 回调、
  stop 串、n_ctx 校验与 prefill/decode 速率回填。CPython 无涉；实测
  10.18 GB `Qwen3.8-27B-UD-IQ3_XXS.gguf`：加载 1.2 s（mmap），24 token 4.7 s（CPU 8 线程）。
  踩坑：ggml 默认 `GGML_OPENMP=ON`，静态库带 `GOMP_*` 引用 → 插件侧必须链接
  `OpenMP::OpenMP_CXX`；llama 头按 SYSTEM 引入以守住零告警基线。
- **`ai_ninfer` 插件（NInfer 引擎专用，※ 无 C ABI）**：取证三条（发行版无头文件/导入库、
  上游明写 headers 不作为 SDK 分发、KVMem/投机解码/视觉只在 server 进程内）⇒ 只能
  「进程监管 + OpenAI HTTP」。注册 `zenith.ai.INinferInference` v1；`backend()` 返回
  `ninfer-http (no C ABI)`，初始化打 WARN，附 `plugins/ai_ninfer/README.md` 专文说明。
  复用 `ai_inference` 的 `engine_supervisor.cpp` / `upstream_http.cpp` /
  `openai_upstream_backend.cpp`（同一份源码，未复制）。
- 服务名互不冲突：`zenith.ai.IInferenceService`（通用）/ `zenith.ai.ILlamaInference` /
  `zenith.ai.INinferInference`，三者可在同一宿主进程共存。
- 新增测试 `test_ai_llama`（真 GGUF 加载 + 生成，缺模型自动 SKIP）、
  `test_ai_ninfer`（真引擎监管 + 状态 + 生成，缺引擎自动 SKIP）。

### M8.1 系列（前序）
- `http::SseStream` / `http::ChunkWriter`（M9.1）：HTTP/1.1 分块流式响应。
  `Context::sse()` 一行起流，事件帧（`event`/`id`/多行 `data`/心跳注释）由框架编码；
  写端线程安全（`TcpConnection::send` 跨线程投递），流式连接不参与 keep-alive 复用，
  空闲踢除自动让路。底层原语 `HttpResponse::chunked` + `http::chunkEncode()`。
- `http::gzipMiddleware`（M7.4）：zlib 后端响应压缩（未探测到 zlib 时整体降级为
  identity，链接不受影响）；`Accept-Encoding` 协商（含 `q=0` 拒绝）、体积阈值、
  204/304/HEAD 跳过、压缩无收益时回退原文。
- `jwt::asym`（M9.2）：RS256/384/512、ES256/384 签发与校验（OpenSSL 后端）。
  ECDSA 按 JWS 输出定长 `R‖S`（非 DER），校验端逆变换；显式拒绝 `none` 与
  `HS*` 防算法混淆；新增 `jwt::headerAlg()` 取 token 头部算法。
- `ai_inference` 插件（M8.1，2026-10 重写）：LLM 推理网关，注册
  `zenith.ai.IInferenceService` v1。**删除进程内 llama.cpp 后端**（发行版均无
  `llama.h`/导入库、源码版本与二进制不一致、KV 均值中心只在 server 层），改为三形态：
  `engine`（监管本地引擎进程）/ `openai`（外部 OpenAI 兼容服务）/ `mock`（确定性，CI 用）。
  配置键 `ai.inference.backend / engine.exe|model|args|env|workdir|ready_line|
  startup_timeout_ms / upstream_url|api_key|timeout_ms / default_model`。
- `EngineSupervisor`（插件内）：`CreateProcess`/`fork+exec` + stdout/stderr 管道 +
  就绪标记探测 + 崩溃重启限次 + Job 对象（`KILL_ON_JOB_CLOSE`，宿主退出不留孤儿进程）
  + **解析引擎控制台统计行**（`req#N done | ... TTFT ... prefill ... decode ... mtp accepted a/b`）。
  实测踩坑两条，均已修：stdin 被重定向时 `hStdInput` 必须是可继承的有效句柄（用 `NUL`）；
  **本机任何非 NULL 的 `lpEnvironment` 都会让 `CreateProcess` 返回 87**（连父进程自己的
  环境块指针也不行），故改为「临时设置父进程环境变量 → 以 NULL 环境块继承 → 立刻还原」。
- `zenith::ai::mountOpenAiApi`（M8.1）：OpenAI 兼容面放核心库，插件只管推理。
  `/v1/models`、`/v1/chat/completions`（`stream=true` 走 SSE + 末片 usage + `[DONE]`）、
  `/v1/completions`、`/v1/embeddings`、`/zenith/ai/status`（网关+引擎运行态）；
  错误体按 OpenAI 形状；参数域收敛；推理经 `options.pool` 投递线程池
  （非流式用 chunked 延迟响应，绝不占 EventLoop）；token 用量与请求数进 Prometheus。
- 引擎遥测（M8.1）：解析响应里的 `timings` 与控制台统计行 →
  `zenith_ai_decode_tps` / `zenith_ai_prefill_tps` / `zenith_ai_ttft_ms` /
  `zenith_ai_draft_tokens_total` / `zenith_ai_draft_accepted_total`（投机接受率）/
  `zenith_ai_engine_running` / `zenith_ai_cached_prompt_tokens_total`。
- `ai_inference` 的 **OpenAI 上游后端**：插件内自带最小同步 HTTP/1.1 客户端
  （chunked + SSE 流式回调 + 超时），本地引擎与云端 API 同走这条路径。
- 思考模型支持：`ChatResult.reasoning` + `chat(..., onToken, onReasoning)` 双回调，
  OpenAI 层分开透出 `reasoning_content`（非流式）与 `delta.reasoning_content`（流式）——
  规避「max_tokens 被思考吃光 → content 为空」的典型现象。
- 新增测试套件 `test_stream`（5 例）、`test_compress`（4 例）、`test_jwt_asym`（5 例）、
  `test_ai_inference`（11 例，含 `ZENITH_TEST_UPSTREAM_URL` / `ZENITH_TEST_ENGINE_EXE`
  门控的两条活体联调：外部上游、监管本地引擎）；
  `test_stream` 以本仓 HttpClient 解分块，服务端写端与客户端解析器互为对照。
- `http::spaHandler(rootDir)`（M5.7）：SPA 静态 + index.html 回退。
  真实文件优先（ETag/304/Range/小文件缓存）；未命中且路径非资源形态
  （末段无扩展名）回 index.html 并带 `Cache-Control: no-cache`；
  带扩展名的 404 资源保持 404。回归测试 `SpaE2E.FallbackAndAssetPriority`。
- 新增回归测试 `Router.RootPathNotMatchedByCatchAll`：固化
  "catch-all `/*path` 不匹配根路径 /" 的规格（SPA 托管须单独注册 `/`）。

### 修复
- Windows 控制台中文乱码：`ConsoleLogger` 构造时自动切换控制台代码页到
  UTF-8（重定向场景自动跳过）。应用层不再需要自行调用 `SetConsoleOutputCP`。
- `static_files.cpp` 源文件内嵌 NUL 字节（小文件缓存路径的字符字面量），
  消除 "null character(s) preserved in literal" 编译警告。

### 文档
- 里程碑状态对齐实际进度：`README.md` 顶部新增「进度总览」表（M1–M6/M7/M8 1-8/M9/计划外/CI/插件数），
  M9 从"进行中"改为已完成，M8 标注 1/8 并说明 M8.1 实装为 4 个插件；
  `docs/PLAN_M8.md` 新增「进度快照（2026-10-21）」与「计划外落地：引擎形态的四个插件」，
  补齐实施顺序的实际状态、代码量（实际 ~3,470 行）与生态影响的当前/目标态区分。
- `README.md` 依赖原则改写：三方准入从「点名白名单」改为「C ABI 边界」——凡提供稳定
  C ABI 的库均可直接接入，不再逐个立项；补齐已接入/已排期依赖表（llama.cpp /
  whisper.cpp / libsrtp / wasmtime / miniz / libmupdf 等）与四道准入闸（接口/许可/可选/隔离）。
- `docs/ARCHITECTURE.md` 新增 §5g「Blog 实战沉淀」：根路径/catch-all 边界、
  spaHandler 回退边界、WS 会话身份挂 `setContext`（禁止裸指针做键）、
  广播文案与接收者无关、SDK 消费形态验证（ORM=OFF + TLS=openssl）等 7 条。

### 变更
- `ZENITH_TLS=openssl` 时额外编入 `http/jwt_asym.cpp`，并以 `ZENITH_HAS_OPENSSL`
  传递到消费者；zlib 探测结果用私有宏 `ZENITH_HAS_ZLIB` 限定实现，公共头不暴露后端细节。
- `docs/PLAN_M8.md` 的 `ai-mcp` 章节按 **MCP 2026-07-28 无状态规范**重写
  （去 `initialize`/`Mcp-Session-Id`、改 `server/discover` + `subscriptions/listen`
  + MRTR + `resultType`/`ttlMs`，删 SSE 续传），估算 1200 → 1500 行；
  PLAN_M8 结尾「17 个插件」订正为 13（5 实装 + 8 规划）。
- 框架首个实战应用（技术博客 + 3D 联机棋室，React 19 + Three.js）迁出为
  独立仓库，改经 `find_package(Zenith CONFIG)` 消费安装好的 SDK——
  「SDK 化消费」形态首次在 Windows/MinGW 全流程验证（含 ORM=OFF 裁剪）。
- 根 CMakeLists 移除 `ZENITH_BUILD_APPS` 选项（apps/ 已随应用迁出）。

### 已知问题
- `ZENITH_TLS=none` 时 net/http 层仍无条件引用 TLS 符号，静态/动态库均
  不可链接；规避：`ZENITH_TLS=openssl`。计划在 none 后端补 stub 后解除。

## v0.1.0 (2026-10)

首个正式版本。六个里程碑、27 个提交、~24,000 行自研代码、22 个测试套件全绿。

### M1 插件内核
- 纯 C ABI v1（`zenith/abi/zenith_abi.h`）：struct_size/abi_version 前向兼容、
  宿主能力函数指针表、生命周期回调；C89/C++、MSVC/GCC/Clang 通用
- ServiceRegistry（接口名+版本协商、owner 安全清理）、PluginManager
  （加载校验、逆序卸载）、DynLib（LoadLibrary/dlopen）
- ThreadPool、Vyukov 无锁有界队列、MPSC 异步滚动日志、可选 C++20 协程层
- 纯 C 插件验证（hello_c 用 C 编译器构建）

### M2 Reactor + HTTP/1.1
- Poller 四后端：WSAPoll / epoll / kqueue / select
- EventLoop（fd 回调、可取消堆定时器、跨线程任务+自连接 UDP 唤醒）
- EventLoopThreadPool、TcpServer/Connection/Client
- 自研 HTTP/1.1 增量解析器（chunked/pipelining）、路由树、Gin 风格 Context、
  洋葱中间件、HttpServer/HttpClient、自研 JSON（RFC 8259）
- `co_await tcpConnect/httpFetch`

### M3 数据层与协议
- WebSocket（RFC 6455：掩码/分片/大帧/close 握手、`enableWebSocket` 一行升级、wss 客户端）
- 自研 Redis RESP2 客户端（增量解析、流水线 FIFO 对齐）
- JsonConfig（dotted/数组/拼平）+ uORM 服务化（连接池/ServiceRegistry/专用线程池）

### M4 生态与边缘
- TLS/OpenSSL：内存 BIO 非阻塞握手、TcpConnection 透明滤层、`listenTls` 一行 HTTPS、
  SNI/证书校验/ALPN；URL 解析 + HttpClient keep-alive 连接池
- media_ffmpeg 插件：probe/snapshot/transcode（CRF/缩放/进度）
- edge_esp32 插件：COBS+CRC32 帧、TCP 网关（hello/心跳/RPC/事件）、
  串口双后端（win32/termios）+ ESP-IDF 固件样例
- ai_comfyui 插件：workflow 提交、{{seed}}/{{key}} 注入、产物下载
- ports/esp-idf 组件脚手架（静态注册表）

### M5 生产就绪
- multipart 上传、静态文件（Range/ETag/穿越防护/小文件缓存）、Session/Cookie、
  优雅停机（在途排空+信号）、wss
- 可观测：Prometheus 指标（直方图 P50/99）、X-Request-Id、`/zenith/status`
- 安全：自研 SHA-256/HMAC（NIST+OpenSSL 交叉核对）、JWT、CORS、令牌桶限流、安全头
- zenith-bench 压测工具；`find_package(Zenith)`；GitHub Actions 三平台 CI；
  zenith-new 脚手架；配置热重载（内容哈希）；多进程 worker（SO_REUSEPORT）；DB 管理台

### M6 发布与性能
- Release 基准：裸 JSON **181K RPS** / HTTPS 154K / soak 271K 热态持续
- 1.64 亿次请求 soak 0 失败、RSS 4.3B/请求无泄漏
- 连接上限 503、keep-alive 空闲踢除、SecurityLimits 收口、装饰器链
- CI 三矩阵全绿（Windows-mingw / Linux-gcc / Linux-clang+coro）

### 已知边界
- aarch64 交叉与 RK/ESP32/ComfyUI 真机验收待硬件
- io_uring/IOCP 后端、HTTP/2、分布式（服务发现/配置中心）规划 M7+
- uORM MySQL 驱动与 ubuntu-24.04 libmariadb3 不兼容（上游问题，CI 走 sqlite）
