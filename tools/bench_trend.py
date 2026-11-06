#!/usr/bin/env python3
"""把一次 perf-gate 的结果追加到历史，并重绘 benchmarks/TREND.md。

用法（CI 每周跑一次，本地也可以手动补录）：
    python3 tools/bench_trend.py --report bench-report.json \
        --history benchmarks/history.jsonl --out benchmarks/TREND.md

为什么用「提交进仓库的 JSONL + Markdown」而不是看板服务：
- 不依赖任何外部服务/凭据，GitHub 上直接可读；
- 历史就在版本库里，时间点/提交号可追溯（记录里带上 git sha）。
"""

import argparse
import datetime as dt
import json
import os
import subprocess
import sys

FIELDS = ["ts", "sha", "file_size_mb", "peak_rss_mb", "big_file_mbps", "json_rps", "ok"]


def git_sha() -> str:
    try:
        return subprocess.check_output(["git", "rev-parse", "--short", "HEAD"],
                                       text=True).strip()
    except Exception:
        return "unknown"


def load_history(path: str) -> list:
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path, encoding="utf-8-sig") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    return rows


def spark(values: list) -> str:
    """把一串数字画成极简条形（不依赖绘图库，GitHub 上也能看）。"""
    if not values:
        return ""
    lo, hi = min(values), max(values)
    blocks = "▁▂▃▄▅▆▇█"
    if hi == lo:
        return blocks[3] * len(values)
    return "".join(blocks[min(7, int((v - lo) / (hi - lo) * 7))] for v in values)


def render(rows: list) -> str:
    lines = [
        "# 性能趋势",
        "",
        "> 由 `.github/workflows/bench-weekly.yml` 每周自动追加，"
        "数据源是 `tools/bench_gate.py` 的同一次测量。",
        "> 机器是 GitHub Actions runner，**绝对值只在同一时段内可比**；"
        "看趋势（是否单调下滑）比看单点更有意义。",
        "",
    ]
    if not rows:
        lines += ["（暂无历史数据）", ""]
        return "\n".join(lines)

    recent = rows[-12:]
    rss = [r.get("peak_rss_mb") for r in recent if r.get("peak_rss_mb")]
    mbps = [r.get("big_file_mbps") for r in recent if r.get("big_file_mbps")]
    rps = [r.get("json_rps") for r in recent if r.get("json_rps")]

    lines += [
        "## 最近 12 次",
        "",
        f"- 大文件吞吐（MB/s）：`{spark(mbps)}`",
        f"- 峰值 RSS（MB）：`{spark(rss)}`  ← 越低越好",
        f"- JSON RPS：`{spark(rps)}`",
        "",
        "| 时间(UTC) | commit | 文件大小 | 峰值 RSS(MB) | 大文件(MB/s) | JSON RPS | 门禁 |",
        "|---|---|---:|---:|---:|---:|---|",
    ]
    for r in reversed(recent):
        lines.append(
            "| {} | `{}` | {}MB | {} | {} | {} | {} |".format(
                r.get("ts", "")[:19].replace("T", " "),
                r.get("sha", ""),
                r.get("file_size_mb", ""),
                r.get("peak_rss_mb") if r.get("peak_rss_mb") is not None else "n/a",
                round(r["big_file_mbps"], 1) if r.get("big_file_mbps") else "n/a",
                r.get("json_rps", "n/a"),
                "✅" if r.get("ok") else "❌",
            )
        )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--report", required=True, help="bench_gate.py --out 产出的 JSON")
    ap.add_argument("--history", default="benchmarks/history.jsonl")
    ap.add_argument("--out", default="benchmarks/TREND.md")
    args = ap.parse_args()

    with open(args.report, encoding="utf-8-sig") as f:
        rep = json.load(f)

    rows = load_history(args.history)
    rows.append({
        "ts": dt.datetime.now(dt.timezone.utc).isoformat(timespec="seconds"),
        "sha": git_sha(),
        "file_size_mb": rep.get("file_size_mb"),
        "peak_rss_mb": rep.get("peak_rss_mb"),
        "big_file_mbps": rep.get("big_file_mbps"),
        "json_rps": rep.get("json_rps"),
        "ok": bool(rep.get("ok")),
    })

    os.makedirs(os.path.dirname(args.history) or ".", exist_ok=True)
    with open(args.history, "w", encoding="utf-8", newline="\n") as f:
        for r in rows:
            f.write(json.dumps({k: r.get(k) for k in FIELDS}, ensure_ascii=False) + "\n")

    with open(args.out, "w", encoding="utf-8", newline="\n") as f:
        f.write(render(rows))

    print(f"history: {len(rows)} 条 → {args.history}")
    print(f"trend: {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
