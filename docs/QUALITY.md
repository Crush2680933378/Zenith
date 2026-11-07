# 质量防线

> README 只放结论，这里是细节。CI 定义见 [.github/workflows/ci.yml](../.github/workflows/ci.yml)。

## 任务矩阵

| 任务 | 拦什么 | 阻塞 |
|---|---|---|
| `windows-mingw` / `linux-gcc` / `linux-clang-coro` / `macos-clang` | 四平台构建 + 全量测试 | ✅ |
| `linux-release` | NDEBUG/优化下才暴露的问题（Debug 全绿 ≠ Release 全绿） | ✅ |
| `linux-sanitize` · ASan+UBSan | 越界/UAF/UB，全量 41 套 | ✅ |
| `linux-sanitize` · TSan | 数据竞争（并发相关 9 套） | ✅ |
| `linux-sanitize` · LSan（零泄漏套件） | 进程退出时的对象泄漏；`test_websocket` / `test_m5_web` 阻塞 | ✅ |
| `linux-sanitize` · LSan（已知时序套件） | `test_m13e` / `test_m13g` 报告，已定位项记在 `.lsan-suppressions.txt` | ⚠️ 报告 |
| `consumer-smoke` | 装 SDK → 用 `find_package(Zenith CONFIG)` 编独立下游项目 | ✅ |
| `perf-gate` | 大文件峰值 RSS + 吞吐下限 | ✅ |
| `coverage` | llvm-cov 行覆盖 + 门限 | ✅ |
| `fuzz-smoke` | 五个解析入口 × 45s | ✅ |
| `Fuzz (nightly)` | 五个目标 × 600s | 夜间 |
| `Flaky (nightly)` | 核心套件重复 3 轮 | 夜间 |

## perf-gate：为什么不比绝对数字

CI runner 的硬件差异比要抓的回归还大——和上一次的绝对数字比，只会天天红或者形同虚设。

改成校验**行为不变量**：

- 64MB 静态文件的峰值 RSS（Linux `VmHWM`）必须 < 64MB
- 大文件吞吐 ≥ 100 MB/s、JSON RPS ≥ 1000

上一轮那个「文件分块堆进发送缓冲 → RSS 265MB、吞吐掉 20%」的回归，正是这条门禁的形状。

本地跑同一条门禁：

```bash
python3 tools/bench_gate.py \
  --server build/bin/zenith_bench_server \
  --bench  build/bin/zenith_bench \
  --out    bench-report.json
```

## sanitizer 上线当轮抓到的问题

| 问题 | 表现 |
|---|---|
| ZTEST 断言宏悬垂引用 | macOS 上 `test_m13b` 段错误；ASan 报 heap-use-after-free |
| crew WS context 类型混淆 | 单槽 `shared_ptr<void>` 被两种类型复用，关闭回调越界读 |
| 插件卸载与在途任务赛跑 | `dlclose` 后任务跳进已释放代码段 |
| HTTP 客户端失败路径循环引用 | 每次超时泄漏一份解析器 + 连接 |
| 空闲定时器强引用连接 | 已关闭连接被多吊 `keepAliveIdleSec` 秒 |
| EventLoop 线程 id 数据竞争 | TSan 在 4 个套件上同时报出 |

## 尚未清零

| 项 | 现状 | 跟踪 |
|---|---|---|
| LSan 已知时序套件 | `test_m13e` / `test_m13g` 在进程退出时仍有对象存活（**非增长型**：测试先退出、服务端还没收尾）。已定位项用 `.lsan-suppressions.txt` 的窄规则固定；其余套件走**阻塞**门禁，新增泄漏会让 CI 变红 | [#2](https://github.com/Crush2680933378/Zenith/issues/2) |
| WebRTC 完整度 | 只有 STUN/ICE 连通性；DTLS/SRTP/SDP/TURN 未实现 | 未开 issue（边界写在 `include/zenith/rtc/stun.hpp`） |

## fuzz 目标

| 目标 | 攻击面 |
|---|---|
| `fuzz_http_request` | HTTP/1.1 请求解析（服务端入口） |
| `fuzz_http_response` | HTTP/1.1 响应解析（恶意上游） |
| `fuzz_ws_frame` | WebSocket 帧解码（长度/掩码/opcode） |
| `fuzz_json` | 自研 JSON 解析器（配置/请求体/上游响应） |
| `fuzz_audio_wav` | WAV 解析/降混/重采样（用户上传音频） |

首跑就抓到：WebSocket 解码器把 RFC6455 保留 opcode（`0x0D`）当正常帧返回。已修并补单测。

本地跑：

```bash
cmake -B build-fuzz -G Ninja -DCMAKE_CXX_COMPILER=clang++ \
      -DZENITH_SANITIZE=address,undefined -DZENITH_BUILD_FUZZERS=ON \
      -DZENITH_BUILD_TESTS=OFF -DZENITH_BUILD_EXAMPLES=OFF -DZENITH_BUILD_PLUGINS=OFF
cmake --build build-fuzz
./build-fuzz/fuzz/fuzz_json -max_total_time=60 fuzz/corpus/fuzz_json
```

## 覆盖率

`coverage` job 用 Clang profile-instr 跑全量测试，llvm-cov 汇总后卡门限
（仓库变量 `COVERAGE_MIN`，默认 80%；当前实测 **85.84%**，排除
`thirdparty/`、`tests/`、`plugins/`、`examples/`）。

## consumer-smoke：为什么需要消费者视角

`consumer-smoke` 装出 SDK，再用 `find_package(Zenith CONFIG)` 编一个独立小项目。
这条链路此前是坏的：导出目标引用了 `ZLIB::ZLIB`，但 config 没 `find_dependency`；
链接项里还带着构建机的绝对路径（`C:/msys64/.../libssl.dll.a`）。只测本仓库永远发现不了。
