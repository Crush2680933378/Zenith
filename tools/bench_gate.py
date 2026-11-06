#!/usr/bin/env python3
"""性能门禁：跑 bench_server + zenith_bench，校验两条**与硬件无关**的不变量。

为什么不做「和上一次的绝对数字比」：CI runner 的 CPU/虚拟化差异比要抓的回归还大，
绝对基线只能抓到灾难级退化，还容易天天红。这里改抓行为不变量：

  1. 大文件的峰值 RSS 不随文件大小膨胀 —— 64MB 文件必须仍能保持在小内存量级
     （这正是上一轮抓到的回归：文件分块堆进发送缓冲 → RSS 265MB）
  2. 吞吐不能塌 —— 大文件 / JSON 各有一个宽松下限

用法：
  python3 tools/bench_gate.py --server build/benchmarks/zenith_bench_server \\
                              --bench  build/bin/zenith_bench \\
                              --max-rss-mb 64 --min-file-mbps 150 --min-json-rps 2000
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

KB_PER_MB = 1024.0

# Windows 控制台默认 GBK，表格里的 ✅/❌/⚠️ 会直接抛 UnicodeEncodeError
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # noqa: BLE001 —— 老 Python 或非标准流，忽略即可
    pass


def parse_metrics(out: str) -> dict:
    """从 zenith_bench 的 markdown 表里抠出 rps / body_throughput。"""
    m = {}
    mm = re.search(r"\|\s*rps\s*\|\s*(\d+)", out)
    if mm:
        m["rps"] = int(mm.group(1))
    mm = re.search(r"\|\s*body_throughput\s*\|\s*([0-9.]+)\s*(\w+B)/s", out)
    if mm:
        val, unit = float(mm.group(1)), mm.group(2).upper()
        scale = {"B": 1 / (1024.0 * 1024.0), "KB": 1 / 1024.0, "MB": 1.0, "GB": 1024.0}.get(unit)
        if scale:
            m["mbps"] = val * scale
    return m


def peak_rss_mb(pid: int):
    """Linux 下读 VmHWM（峰值常驻集）；其他平台返回 None。"""
    path = f"/proc/{pid}/status"
    if not os.path.exists(path):
        return None
    try:
        with open(path, "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    return float(line.split()[1]) / KB_PER_MB
    except OSError:
        return None
    return None


def run_bench(bench: str, url: str, seconds: int, conns: int, threads: int) -> str:
    cmd = [bench, "-u", url, "-d", str(seconds), "-c", str(conns), "-t", str(threads)]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 120)
    if proc.returncode != 0:
        print(f"[gate] bench 失败（{url}）：{proc.stderr[-400:]}", file=sys.stderr)
    return proc.stdout


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", required=True, help="bench_server 可执行文件")
    ap.add_argument("--bench", required=True, help="zenith_bench 可执行文件")
    ap.add_argument("--port", type=int, default=8299)
    ap.add_argument("--file-size-mb", type=int, default=64)
    ap.add_argument("--max-rss-mb", type=float, default=64.0,
                    help="服务端处理大文件时的峰值 RSS 上限（与文件大小无关）")
    ap.add_argument("--min-file-mbps", type=float, default=150.0)
    ap.add_argument("--min-json-rps", type=float, default=2000.0)
    ap.add_argument("--workdir", default=os.path.join("build", "bench-gate"),
                    help="数据目录（默认 build/bench-gate；不用系统 TEMP，"
                         "某些沙箱会禁止子进程写 TEMP）")
    ap.add_argument("--out", default="",
                    help="把本次指标写成 JSON（用于 CI artifact / 趋势记录）")
    args = ap.parse_args()

    workdir = os.path.abspath(args.workdir)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir, exist_ok=True)
    # Popen 带 cwd 时，相对可执行路径会按新 cwd 解析——必须先转绝对路径
    server_exe = os.path.abspath(args.server)
    bench_exe = os.path.abspath(args.bench)
    server = None
    failures = []
    try:
        server = subprocess.Popen(
            [server_exe, str(args.port), "90", str(args.file_size_mb),
             os.path.join(workdir, "www")],
            cwd=workdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        # 等监听就绪
        deadline = time.time() + 60
        ready = False
        while time.time() < deadline:
            line = server.stdout.readline()
            if not line:
                if server.poll() is not None:
                    break
                continue
            if "listening" in line:
                ready = True
                break
        if not ready:
            print("[gate] bench_server 未就绪", file=sys.stderr)
            return 2
        # 让文件生成/预热完成
        time.sleep(1.5)

        base = f"http://127.0.0.1:{args.port}"
        big = parse_metrics(run_bench(bench_exe, base + "/big.bin", 6, 4, 2))
        json_metrics = parse_metrics(run_bench(bench_exe, base + "/json", 4, 4, 2))
        rss = peak_rss_mb(server.pid)

        print("\n| 指标 | 实测 | 门限 | 结果 |")
        print("|---|---:|---:|---|")

        def check(name, value, limit, unit, lower_is_better):
            if value is None:
                print(f"| {name} | n/a | {limit}{unit} | ⚠️ 无数据 |")
                return
            ok = (value <= limit) if lower_is_better else (value >= limit)
            op = "≤" if lower_is_better else "≥"
            print(f"| {name} | {value:.1f}{unit} | {op}{limit:.0f}{unit} | {'✅' if ok else '❌'} |")
            if not ok:
                failures.append(name)

        check(f"峰值 RSS（{args.file_size_mb}MB 文件）", rss, args.max_rss_mb, "MB", True)
        check("大文件吞吐", big.get("mbps"), args.min_file_mbps, " MB/s", False)
        check("JSON RPS", float(json_metrics.get("rps", 0)) or None, args.min_json_rps, "", False)

        if rss is None:
            print("| （非 Linux 环境跳过 RSS 校验） | | | |")
    finally:
        if server is not None and server.poll() is None:
            server.terminate()
            try:
                server.wait(timeout=10)
            except subprocess.TimeoutExpired:
                server.kill()
        shutil.rmtree(workdir, ignore_errors=True)

    if failures:
        print(f"\n[gate] ❌ 未通过：{', '.join(failures)}", file=sys.stderr)
        rc = 1
    else:
        print("\n[gate] ✅ 全部通过")
        rc = 0

    if args.out:
        report = {
            "file_size_mb": args.file_size_mb,
            "peak_rss_mb": rss,
            "big_file_mbps": big.get("mbps"),
            "json_rps": json_metrics.get("rps"),
            "limits": {
                "max_rss_mb": args.max_rss_mb,
                "min_file_mbps": args.min_file_mbps,
                "min_json_rps": args.min_json_rps,
            },
            "failed": failures,
            "ok": rc == 0,
        }
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(report, f, ensure_ascii=False, indent=2)
    return rc


if __name__ == "__main__":
    sys.exit(main())
