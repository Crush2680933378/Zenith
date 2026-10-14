# Zenith M5 计划 —— 生产就绪（框架完整性 / 安全 / 可观测 / 性能 / 交付）

> 前置状态：M1 插件内核 ✅ · M2 Reactor+HTTP ✅ · M3 WebSocket/Redis/配置/DB ✅
> · M4 TLS/媒体/设备桥/AI/连接池 ✅（aarch64 实机与真机验收随硬件）
> 当前基线：17/17 测试套件全绿，16,000+ 行自研，零三方依赖（除 uORM/OpenSSL/FFmpeg）。

## 总原则

M1–M4 回答"能做什么"；M5 回答**"能不能扛生产"**：
完整度（真实 Web 必需的缺口）、安全（默认基线）、可观测（出了事看得见）、
性能（有数字、有回归门槛）、交付（别人能装能用）。全部自研，沿用既有
ZTEST + 端到端 + 压测验收标准；新能力仍以"核心接口 + 插件形态"分层落地。

---

## M5.1 Web 框架完整性补全（前置，其余多依赖它）

**缺口清单（M2–M4 遗留）**
- **multipart/form-data 解析器**：文件上传（边界扫描、分块流式、大小上限、
  临时文件落盘策略）——`Context::formData()` / `file("field")` API；
  media 示例的"上传转码"补全为真正的表单上传。
- **静态文件服务**：`server.static_("/assets", rootDir)`——Content-Type 映射表、
  ETag/Last-Modified 协商缓存、`Range` 断点续传（单段/多段）、目录穿越防护；
  大文件零拷贝通道抽象（Windows `TransmitFile` / POSIX `sendfile`，接口
  `ISendFileChannel` 便于后续扩展 io_uring）。
- **Session/Cookie**：服务端 Session 抽象（ISessionStore：内存实现 + Redis 后端
  ——复用 M3 自研 RESP 客户端）；Cookie 签名（复用 crypto，扩展 SHA-256/HMAC）。
- **优雅停机**：信号处理（Windows console Ctrl / POSIX SIGTERM/SIGINT）→
  停 accept → 通知插件 stop → 排空在途请求（带 deadline）→ 关闭；
  02_http_server 示例去掉"Ctrl+C 由后续里程碑补齐"注释。
- **wss 确认**：WS over TLS 组合验证（M3 溯 M4 滤层应天然生效）+ 测试收编。

**验收**：multipart 混合表单/大文件分块上传/恶意边界用例；静态文件 curl
`-H Range` 三种形态 + ETag 304；session Redis 后端跨进程；kill -TERM 后
在途请求全部完成（日志断言 drain）；wss 端到端。

## M5.2 安全中间件族（默认开箱基线）

- **CORS 中间件**：白名单/预检缓存/凭据透传。
- **限流**：令牌桶（原子实现）+ 键控（IP/路由/用户）+ `Retry-After`；
  分布式限流预留（Redis 计数后端）。
- **JWT**：自研 HMAC-SHA256（crypto 模块补 SHA-256 + HMAC + 恒时比较）；
  签发/校验中间件、过期/签名错误语义；示例：/login → token → 保护路由。
- **安全响应头**：CSP/X-Frame-Options/HSTS（TLS 站点）一键中间件。
- **body 防护强化**：现有上限参数收编为 `SecurityLimits` 统一配置。

**验收**：每件中间件独立单测 + 组合端到端（登录→限流触发→429→CORS 头断言）；
SHA-256/HMAC 用 NIST 标准向量核对（重演 M4 对 SHA-1 的权威核对法）。

## M5.3 可观测性

- **指标**：`/metrics`（Prometheus 文本格式，自研序列化）——QPS/P50·P95·P99
  延迟直方图（HDR 简化版）、连接数、EventLoop 队列深度、线程池水位、
  连接池命中/miss、插件注册表快照。
- **请求追踪**：X-Request-Id 中间件（生成/透传）+ 日志字段注入 + 响应回写；
  跨 HttpClient 调用透传（为后续跨服务链路预留 `trace` 头族）。
- **运行时状态**：`/zenith/status`（JSON：模块/插件/线程/池/定时器计数）；
  `/zenith/plugins`（列表+启停）。
- **日志结构化**：可选 JSON 行格式（采集友好），与文本格式开关切换。

**验收**：本地起服务打流量 → curl /metrics 输出合法（prometheus 格式自校验）；
Request-Id 全链路唯一且日志可 grep 串联；压测中 status 接口自身开销 < 1%。

## M5.4 性能压测与调优（依赖 M5.1）

- **自研压测工具 `zenith-bench`**：多线程 RPS/延迟统计（复用自家 HttpClient 池
  与 WS），不依赖 wrk/ab——与"全自研"原则一致；输出 markdown 报告。
- **基准场景固定化**（回归门槛进 CI）：echo / JSON API / 静态文件小/大 /
  WS 广播 / HTTPS 握手；记录 Windows/Linux 基线表，劣化 >10% 失败。
- **热路径调优**：
  - Buffer 池化（对象池，削热路径分配）
  - 路由匹配节点缓存（method+pattern → handler 索引）
  - 日志格式化零分配快路径
  - （可选）TimerWheel 替换堆定时器（海量短定时器场景）
- **内存红线**：长稳测试（1h 压测）RSS 增幅 < 5%（检泄漏）。

**验收**：bench 工具产出四个场景报告；至少一轮"调优前后"对比数据入 docs；
CI 基线门槛生效。

## M5.5 交付工程化

- **CMake install/export**：`find_package(Zenith CONFIG)` 可用；安装布局
  （include/lib/cmake）；zenith_coro/tls 按选项导出。
- **CI 流水线**（GitHub Actions）：windows-msvc / windows-mingw / linux-gcc /
  linux-clang × {debug, coro}，跑 ctest + bench 门槛；子模块代理走 7897 的
  组织级配置说明。
- **脚手架**：`zenith-new`（脚本）生成新工程骨架（CMake + main + 路由示例 +
  配置文件 + .gitignore）——降低接入门槛。
- **文档站**：docs/ 扩展（API 参考、插件开发指南、部署手册、性能指南），
  README 瘦身为入口。

**验收**：新机/新目录 `find_package` 消费成功；CI 全绿徽章；脚手架生成的
工程开箱即跑。

## M5.6（可选，排期富余才做）

- 多进程 worker（POSIX SO_REUSEPORT）+ 主进程管理
- 配置热重载（SIGHUP / 文件监听）
- io_uring 后端（Linux，ITransport 第三形态）
- uORM Web 管理台迁移到 Zenith 自研 HTTP 上承载（M3 起的旧愿）

---

## 顺序与依赖

```
M5.1 完整性 ──→ M5.4 性能（静态文件/上传场景就位才能压）
        ├────→ M5.2 安全（session 依赖 5.1 的 cookie 基建）
        └────→ M5.3 可观测（路由维度统计依赖 5.1 收口的路由缓存）
M5.5 交付（任意时点可做，建议与 5.4 并行推进）
M5.6 可选
```

建议实施顺序：**5.1 → 5.3 → 5.2 → 5.4 → 5.5 →（5.6 可选）**。
每个子阶段独立提交、独立验收；硬件联调项（aarch64/真机 ComfyUI/ESP32）
随硬件到位穿插进行，不阻塞主线。

## 风险

- 静态文件零拷贝在 Windows（TransmitFile）与非阻塞集成的复杂度——首版允许
  退化为常规 send，通道抽象保住演进面。
- 性能门槛定太松无意义、太紧误伤——基线先行，门槛取"劣化>10%"相对值。
- SHA-256 自研的安全敏感度高于 SHA-1——实现后除 NIST 向量外，追加与
  OpenSSL 输出的随机数据交叉核对（万组样本）。
