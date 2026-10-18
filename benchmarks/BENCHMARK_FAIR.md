# Zenith 跨语言基准报告（2026-10-18）

## 公平性条款

1. **同机同客户端**：全部服务端跑本机（Win10 x64 / loopback），统一用 zenith-bench
   （2 客户端线程 × 8 keep-alive 链）；服务端框架零业务逻辑——固定文本 + 动态 JSON
2. **预热 3s** 淘汰 JIT/缓存冷启动，随后**每端点 3 轮取中位数**（表内 rps_median）
3. **同构建档**：Zenith RelWithDebInfo(-O2)、Rust release(-O3)、Go 默认(gc)、
   Java/Node/Python 各自默认 JIT
4. **内存**为压测中稳定 RSS（WorkingSet）
5. 局限：客户端即 zenith-bench（自研）—— 30 万 RPS 段可能已接近客户端/loopback
   天花板（双方共享同一物理机网卡栈），更高速差需专用压测机验证

## 版本矩阵

| 语言/框架 | 版本 |
|---|---|
| C++ / Zenith | GCC 16.1 (MinGW-w64) / Zenith v0.1.0 |
| Rust / axum | Rust 1.98.1 / axum 0.7 |
| Go / stdlib + gin | Go 1.23.4 / gin v1.10.0 |
| Java / JDK HttpServer、Spring Boot | OpenJDK 17.0.19 / Spring Boot 3.2.5 |
| Node.js / node:http、express、fastify | v24.16.0 / express 5.x、fastify 5.x |
| Python / FastAPI | 3.14.5 / FastAPI + uvicorn |

## 结果（RPS 中位数，10s×3 轮；P99 均 ≤1ms 除 Python）

| 框架 | plaintext RPS | JSON RPS | RSS(MB) | 相对 Zenith |
|---|---:|---:|---:|---:|
| **Zenith (C++17)** | **237,952** | **234,459** | **9.2** | 1.00× |
| Rust (axum) | 153,043 | 153,218 | 7.1 | 0.64× |
| Go (gin) | 145,471 | 139,426 | 12.2 | 0.61× |
| Java (Spring Boot) | 133,369 | 123,164 | 156.5 | 0.56× |
| Java (JDK HttpServer) | 130,172 | 129,299 | 52.1 | 0.55× |
| Go (stdlib) | 128,422 | 116,097 | 8.4 | 0.54× |
| Node (node:http) | 88,537 | 89,017 | 48.2 | 0.37× |
| Node (fastify) | 85,217 | 83,211 | 57.5 | 0.36× |
| Node (express) | 60,078 | 55,990 | 51.9 | 0.25× |
| Python (FastAPI) | 3,902 | 2,993 | 53.6 | 0.02× |

## 关键结论

1. **Zenith 以 238K plaintext / 234K JSON RPS 居首**——超 axum 55%、gin 63%、
   Spring Boot 78%、express 4×；Python FastAPI（CPython）慢 2 个数量级
2. **内存最低梯队**：Zenith 9.2MB、Rust 7.1MB、Go 8.4MB——比 Spring Boot
   （156.5MB）**少 17 倍**、比 Node 系（48-58MB）少 5-6 倍
3. **P99 全部 ≤1ms**（Zenith/Rust/Go/Java/Node 在同客户端精度下不可区分；
   Python P99 5-10ms——解释型开销可见）
4. **客户端瓶颈注记**：238K 段双方共占同一物理机（客户端+服务端+loopback 栈），
   真实差距可能更大——若在专用压测机（如 Linux + wrk/vegeta）上复测，
   C++/Rust/Go 领先梯队相对位次预期不变
5. **Go gin > Go stdlib**（plaintext）：gin 的 radix 路由确实快于 stdlib ServeMux
6. **Spring Boot vs JDK HttpServer**：plaintext 快 2%、JSON 慢 5%——Spring 的
   开销主要在启动/内存（3× RSS）而非稳态吞吐（JIT 后序列化摊薄）

## 复现

```bash
cd C:\bench
python run.py 10 3          # 全量（10s×3 轮）
# 或单框架: zenith_bench.exe -u http://127.0.0.1:PORT/hello -d 10 -c 8 -t 2
```

各服务端源码同目录：ZenithBench.java / bench.go / ginb/main.go /
node_{fastify,express,http}.mjs / py_fastapi.py / rustb/src/main.rs
