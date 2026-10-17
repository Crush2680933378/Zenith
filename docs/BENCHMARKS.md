# Zenith 性能基线（zenith-bench）

## 环境

- Windows 10 x64 / MinGW-w64 GCC 16.1 / Debug 构建（无优化）
- 服务端：`examples/02_http_server`（路由参数 + JSON 序列化，IO 线程池 4）
- 客户端：`zenith-bench`（2 线程 × 8 keep-alive 链，loopback）
- 命令：`zenith_bench -u http://127.0.0.1:8080/api/users/42 -d 5 -c 8 -t 2`

## 基线（2026-10，M5.4）

| metric | value |
|---|---|
| requests(ok) | 258,117 |
| requests(fail) | 0 |
| rps | **30,970** |
| p50 | ≤0.001s |
| p90 | ≤0.001s |
| p99 | ≤0.001s |
| body_throughput | 1.45 MB/s |

## Release 基线（2026-10，M6.2，RelWithDebInfo / -O2 / loopback / 2线程×8链）

| 场景 | RPS | P99 | 备注 |
|---|---|---|---|
| 裸 JSON API（无日志中间件） | **181,716** | ≤1ms | http_server.hpp 直接起服务 |
| 裸 text / HTTPS hello | 154,232 | ≤1ms | TLS 握手摊薄后几乎零开销 |
| JSON + **同步控制台访问日志** | 30,477 | ≤1ms | ⚠️ 控制台日志是 6× 瓶颈 |
| 静态 5KB 文件（含小文件缓存） | 7,164 | 2ms | ~35MB/s；缓存对热磁盘无增益（瓶颈在拷贝链路） |

**核心结论（M6.2 实测）**：生产环境访问日志必须走 AsyncFileLogger（M1 已有），
勿用同步 ConsoleLogger——同步控制台输出造成 6 倍吞吐损失。
Buffer 池化经评估暂缓：连接级 Buffer 常驻复用，每请求分配在 Context/字符串层，
181K RPS 下非瓶颈（留待 500K+ 目标时再议）。

## 长稳（M6.3 soak，2026-10）

| 指标 | 值 |
|---|---|
| 时长 / 请求量 / 失败 | 10min / **163,966,901** / **0** |
| 持续 RPS | 271,578（热态高于短测 181K） |
| RSS | 9.5MB → 10.2MB（+714KB / 1.64 亿次 ≈ **4.3 B/请求**） |

第二窗口复测（新进程 2min）：+504KB 后趋平（35M 请求 ≈ 14 B/请求，含启动建链）。
**结论：无泄漏迹象**——增量与分配器 arena 收敛一致（基础 RSS 仅 ~9.5MB，
5% 红线在此低基数下过于敏感；1% 绝对阈值或稳态二窗斜率更适合小内存进程）。

## 回归门槛（CI）

同环境复跑劣化 > 10% 视为回归（PR 拦截）。Release 构建预期 2~4× 提升，
待 CI 矩阵补 Release 位后另立 Release 基线。
