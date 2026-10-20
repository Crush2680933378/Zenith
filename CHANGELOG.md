# Changelog

## Unreleased

### 新增
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
- `docs/ARCHITECTURE.md` 新增 §5g「Blog 实战沉淀」：根路径/catch-all 边界、
  spaHandler 回退边界、WS 会话身份挂 `setContext`（禁止裸指针做键）、
  广播文案与接收者无关、SDK 消费形态验证（ORM=OFF + TLS=openssl）等 7 条。

### 变更
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
