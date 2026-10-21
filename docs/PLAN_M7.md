# Zenith M7 计划 —— 性能深挖（热路径 / I/O 后端 / 协议 / 内存）

> 前置：M1–M6 完成，v0.1.0 已发布（GitHub Release 双平台包）。
> 当前基线：裸 JSON 181K RPS（Win/WSAPoll）、soak 1.64 亿请求 0 失败、
> 内存 9.2MB、P99 ≤1ms、跨语言 10 框架基准居首。
> M7 目标：**181K → 300K+（Win）/ 250K+（Linux epoll）**，同时补齐协议层关键缺口。

---

## M7.1 热路径微优化（预估 +8-12%，1-2 天）

### 1a. eventfd 替代 UDP 唤醒（Linux）
- **现状**：`net_detail.cpp` 用自连接 UDP socket 跨线程唤醒 EventLoop——
  每次唤醒走完整 UDP 协议栈（IP 头 + UDP 头 + 校验和）
- **目标**：Linux 上用 `eventfd()`——8 字节内核计数器，天然与 epoll 集成
  （`EPOLLIN` 直接触发），零协议开销
- **预估**：+3-5%（跨线程任务派发频度高时）
- **实现**：`#ifdef ZENITH_PLATFORM_LINUX` 分支替换 `createWakeupFd()`；
  Windows 保留 UDP 方案（无 eventfd 等价物）

### 1b. HTTP 常用 header 预编码字节串
- **现状**：`HttpResponse::serialize()` 每次拼接 `Content-Length: `、
  `Content-Type: text/plain; charset=utf-8`、`Connection: keep-alive` 等
- **目标**：预编码为 `std::string_view` 常量表，直接 append 字节段
- **预估**：+5%（小响应场景头部占比高）
- **实现**：`static const std::string kPreContentLength = "Content-Length: ";` 等

### 1c. NUMA 线程绑定
- **现状**：EventLoopThreadPool 的 IO 线程无亲和性，OS 调度器可任意迁移
- **目标**：`pthread_setaffinity_np` 按 worker 索引绑核（Linux/macOS）；
  Windows 用 `SetThreadAffinityMask`
- **预估**：大规模多核场景 +3-8%（L1/L2 缓存命中率提升）

---

## M7.2 请求生命周期内存优化（预估 +15-25%，2-3 天）

### 2a. Context 对象池
- **现状**：每 HTTP 请求堆分配 `HttpRequest` + `HttpResponse` + `Context`
  三个对象 + 内部 string/map——181K RPS 下分配器压力可见
- **目标**：thread-local free list 池化——请求完成后 Context 归还池中，
  下次请求直接复用（析构后重建 in-place）
- **注意**：归还在 EventLoop 线程（与分配同线程）——无需锁；需确保所有
  shared_ptr 引用清零后才归还
- **实现**：
  ```cpp
  class ContextPool {
      std::vector<std::unique_ptr<Context>> free_;
      static ContextPool& local(); // thread_local
  public:
      std::unique_ptr<Context> acquire();   // 取或新建
      void release(std::unique_ptr<Context>); // 归还（上限 256）
  };
  ```

### 2b. Arena 分配器（请求内临时分配）
- **现状**：请求处理中 handler 里的局部 string/vector 走全局堆
- **目标**：请求生命周期内所有临时分配走 arena（预分配 4KB 块链），
  响应发送后整链回收——零碎片零逐个 free
- **实现**：`core::Arena` 类；`Context::arena()` 暴露给 handler 使用；
  JsonValue 序列化路径改走 arena
- **预估**：与 2a 合计 +15-25%

### 2c. 连接 Buffer 动态缩容
- **现状**：TcpConnection 的 inBuf_/outBuf_ 初始 1KB，高峰可扩到 64KB+，
  空闲后不收缩
- **目标**：空闲 60 秒后 shrink_to_fit 到 256B（keep-alive 长连接场景
  大量空闲连接省内存）

---

## M7.3 静态文件零拷贝（静态场景 2-3×，2 天）

- **现状**：磁盘 → read() 用户态 → send() 内核态 → 网卡，四次拷贝
- **目标**：
  - Windows：`TransmitFile()`（内核直接 TCP ← 文件句柄）
  - Linux：`sendfile()`（同上）；或 `mmap()` + `writev()`（小文件）
- **接口已留**：M5.1 staticHandler 返回前 body 是完整 string——改造为
  `struct FileResponse { int fd; uint64_t offset; uint64_t len; }`，TcpConnection
  识别后走零拷贝通道
- **限制**：Range 请求需回退到用户态（offset 简单但 sendfile 不支持
  等价于 read+seek 组合的场景需评估）

---

## M7.4 响应压缩（deflate/gzip，3-4 天）

> **状态（2026-10）**：已按 **zlib 后端**形态落地 —— `http::gzipMiddleware`（M9.1 同批）。
> 依赖原则已改为「C ABI 即准入」，zlib 属于纯 C ABI 依赖；zlib 未探测到时自动降级为
> identity，自研 deflate 由"必做项"降为"可选练习"。

- **自研 deflate**（兼容 zlib wire format）：
  - 固定 Huffman 编码（快速路径）+ 动态 Huffman（高压缩率路径）
  - LZ77 滑动窗口 32KB
  - 约 1500 行 C++，无三方依赖（符合"能自研全自研"原则）
- **集成**：`Context::compressed()` —— 检查 Accept-Encoding 自动选择
  gzip/identity；中间件形式全局启用
- **预估**：JSON API 响应 60-80% 体积缩减；带宽受限场景吞吐提升显著
- **测试**：与 `gzip -d` 命令行交叉核对随机数据（重演 SHA-256 的
  OpenSSL 交叉验证法）

---

## M7.5 io_uring 后端（Linux 5.1+，可选，大）

- **前提**：需 Linux 裸机（非 WSL2）；`liburing-dev` 或裸 syscall
- **收益**：高并发短连接场景 30-50%——系统调用批量提交（一次
  `io_uring_enter` 提交 N 个操作）、完成队列无锁、真正的异步 accept/read/write
- **实现**：
  - 新增 `PollerIoUring` 类，实现同一 `Poller` 接口（registerFd/updateFd/
    deregisterFd/poll）
  - `io_uring_setup` + SQ/CQ mmap；`IORING_OP_READ/WRITE/ACCEPT`
  - 与 EventLoop 集成：`io_uring_enter` 替代 `epoll_wait`，
    CQ 事件直接触发 fd 回调
- **接口位已留**：`ITransport` 第三形态（M5.6 裁剪时预留）
- **风险**：内核版本差异大；需要真实 Linux 环境长期验证

---

## M7.6 IOCP 后端（Windows，可选，大）

- **收益**：Windows 原生 Proactor——比 WSAPoll（level-triggered 轮询）
  更高效，尤其高并发连接
- **实现**：`CreateIoCompletionPort` + `GetQueuedCompletionStatusEx`
  批量取完成事件；OVERLAPPED I/O
- **接口**：与 Reactor 的 `Poller` 不同（Proactor 模型），走独立的
  `ITransport` 抽象——操作发起即异步，完成由 IOCP 回调

---

## M7.7 HTTP/2（可选，很大，~5000 行）

- HPACK 头部压缩（静态表 61 条 + 动态表 + 霍夫曼编码）
- 帧解析器（DATA/HEADERS/PRIORITY/RST_STREAM/SETTINGS/PING/GOAWAY/
  WINDOW_UPDATE/CONTINUATION）
- 流控（发送端/接收端窗口）
- 多路复用（单 TCP 连接并发 N 流）
- 优先级与依赖树
- **前置**：M7.4 压缩（HPACK 霍夫曼复用 deflate 思路）

---

## 顺序与依赖

```
M7.1 微优化 ──→ M7.2 内存池化 ──→ M7.3 零拷贝 ──→ M7.4 压缩
                                                      │
M7.5 io_uring（需 Linux 裸机）←── 独立可并行 ──────────┤
M7.6 IOCP（独立）                                    │
M7.7 HTTP/2 ←── 依赖 M7.4 的霍夫曼 ──────────────────┘
```

建议：**7.1 → 7.2 → 7.3 → 7.4 为主线**（合计约 1-2 周，181K → 250K+ 预期）；
7.5-7.7 视硬件/需求穿插。

---

## 基线与回归门槛

| 指标 | M6 基线 | M7 目标 | 回归红线 |
|---|---|---|---|
| 裸 JSON RPS (Win) | 181,716 | 250,000+ | <160,000 |
| 裸 JSON RPS (Linux) | ~50K (WSL2 限制) | 待裸机验证 | — |
| soak RSS 增幅 | 4.3B/请求 | <2B/请求 | >8B/请求 |
| 静态文件 RPS | 7,164 | 20,000+ | <5,000 |
| 测试套件 | 22/22 | 22/22+N | 任何失败 |
