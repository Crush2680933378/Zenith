# 性能趋势

> 由 `.github/workflows/bench-weekly.yml` 每周自动追加，数据源是 `tools/bench_gate.py` 的同一次测量。
> 机器是 GitHub Actions runner，**绝对值只在同一时段内可比**；看趋势（是否单调下滑）比看单点更有意义。

## 最近 12 次

- 大文件吞吐（MB/s）：`▄`
- 峰值 RSS（MB）：`▄`  ← 越低越好
- JSON RPS：`▄`

| 时间(UTC) | commit | 文件大小 | 峰值 RSS(MB) | 大文件(MB/s) | JSON RPS | 门禁 |
|---|---|---:|---:|---:|---:|---|
| 2026-11-08 23:15:12 | `5df993c` | 64MB | 29.6875 | 574.4 | 22049 | ✅ |
