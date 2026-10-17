# Zenith M6 计划 —— v0.1.0 发布与性能收敛

> 前置：M1–M5.6 全部完成（14 提交 / 175 文件 / 21 套件全绿 / 31K RPS Debug 基线）。
> 能力面已闭环；M6 的主题是**把它变成一个"发得出去、扛得住、装得上"的 v0.1.0**。

## 环境盘点（2026-10）

- WSL2 已注册 Ubuntu-24.04 但 **Hyper-V 未启用**（启动失败）→ Linux 侧闭环需用户
  管理员开启（`bcdedit /set hypervisorlaunchtype auto` + 重启），或提供任意
  Linux 机器/Docker。
- gh 已认证（Crush2680933378）；GitHub 推送为外发动作，**需用户确认目标仓库后执行**。
- aarch64 交叉工具链未装；RK/ESP32/ComfyUI 真机验收等硬件。

## M6.1 GitHub 发布（需确认）

- `gh repo create Zenith`（public/private 由用户定）+ push 全部 14 提交
- 首次真实 CI 运行：win-mingw / linux-gcc / linux-clang+coro 三矩阵
  （linux 作业在 Actions runner 上跑——**顺带完成 epoll 后端、POSIX 信号、
  WorkerSupervisor 的首次真实编译+测试**，一举闭环 M2/M5.6 的 Linux 侧验证）
- README 徽章（CI/版本/许可证）、LICENSE 文件定稿（MIT，与 uORM 一致）

## M6.2 Release 基准 + 热路径调优

- `win-mingw-release` preset 跑 bench 四场景（echo/JSON/静态/WS 广播），
  预期 2~4×——Release 基线另立一行入 BENCHMARKS.md，Debug 基线保留回归
- **Buffer 池化**：`core::Buffer` 对象池（thread-local free list，容量分档），
  削 HTTP 热路径分配；bench 前后对比佐证
- **静态文件小文件缓存**：≤64KB 文件内存缓存（ETag 失效），静态场景收益验证
- （可选）短定时器批量到期：EventLoop 的 timers_ 堆在万级定时器下的
  批量 pop 优化；bench 定时器场景先行，无瓶颈则记录后跳过

## M6.3 稳定性长稳

- 1 小时压测（bench + interval 请求混合负载）：RSS 增幅 < 5% 红线
- `/zenith/status` 增补：fd 计数、连接池命中率、EventLoop 待处理任务数
  （排查泄漏的一线指标）
- 发现泄漏即修（历史经验：多为回调闭包持有连接）

## M6.4 HTTP 协议补强（生产缺口）

- **服务端 keep-alive 空闲超时**（默认 60s）与**最大并发连接数**限制
  （默认 10K，超出新连接即 503）——slowloris 类慢连接攻击的基本防线
- `SecurityLimits` 统一收口（M5.2 遗留 TODO）：header 数/URL 长度/主体/闲置
  超时集中一处配置，Context/HttpServer 各处散装上限参数归并
- `Expect: 100-continue` 支持（大文件上传客户端兼容）

## M6.5 v0.1.0 发布

- 版本号 0.1.0、CHANGELOG.md（六里程碑汇总）、git tag `v0.1.0`
- GitHub Release（附 Windows 预编译包：zenith.dll/libzenith.a + 头文件 zip）
- 文档终审：README 快速上手路径走通（新机视角）

## M7+ 候选（方向预告，另立计划）

- **分布式**：服务注册发现（复用自研 Redis 客户端）、配置中心集群同步、
  JSON-RPC 调用层、熔断中间件
- **协议**：HTTP/2（hpack 自研）、服务端 gzip（ deflate 自研工作量大，评估 miniz 级裁剪）
- **io_uring / IOCP**：Linux 真机后；接口位已留
- **硬件联调**：RK3566/RK3576（aarch64 交叉）、ESP32 刷机、ComfyUI 真实出图

## 顺序与依赖

```
M6.1 发布(需确认) ──→ M6.2 Release 基准 ──→ M6.3 长稳 ──→ M6.5 tag
                └────→ M6.4 协议补强（并行，任意时点）
硬件/Linux 环境就绪 → 并行插入对应验证，不阻塞主线
```

## 风险

- Release 下未定义行为现身（Debug 掩盖）：以 bench+ctest Release 双跑覆盖
- Buffer 池化引入归还竞态：沿用"仅 loop 线程触碰"红线 + TSAN 级审查
- GitHub 发布范围（public？仓库名？）：待用户拍板
