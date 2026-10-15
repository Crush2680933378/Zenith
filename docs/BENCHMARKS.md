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

## 回归门槛（CI）

同环境复跑劣化 > 10% 视为回归（PR 拦截）。Release 构建预期 2~4× 提升，
待 CI 矩阵补 Release 位后另立 Release 基线。
