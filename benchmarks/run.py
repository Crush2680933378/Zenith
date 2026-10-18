#!/usr/bin/env python3
"""跨语言基准驱动（Python 版——绕开 bash/CRLF/awk 引用地狱）"""
import subprocess, time, re, os, sys, signal

BENCH = r"C:\Users\26809\Desktop\Code\c++\Zenith\build\rel2\bin\zenith_bench.exe"
DUR = int(sys.argv[1]) if len(sys.argv) > 1 else 10
ROUNDS = int(sys.argv[2]) if len(sys.argv) > 2 else 3

SERVERS = [
    ("Zenith(C++17)",        [r"C:\...\srv_bare.exe"], 8096),  # placeholder 修复在下
]

def run_bench(url, dur, warm=False):
    r = subprocess.run([BENCH, "-u", url, "-d", str(dur), "-c", "8", "-t", "2"],
                       capture_output=True, text=True, timeout=dur+60)
    out = r.stdout.replace("\r", "")
    d = {}
    for line in out.split("\n"):
        m = re.match(r"\|\s*(.+?)\s*\|\s*([\d.]+)\w*\s*\|", line)
        if m:
            d[m.group(1)] = m.group(2)
    return d

def rss_mb(pid):
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-Command",
                            f"(Get-Process -Id {pid} -EA SilentlyContinue).WorkingSet64"],
                           capture_output=True, text=True, timeout=15)
        v = r.stdout.strip().rstrip("\r")
        return round(int(v) / 1048576, 1) if v else 0
    except Exception:
        return 0

SERVERS = [
    ("Zenith(C++17)",        [r"C:\Users\26809\Desktop\Code\c++\Zenith\build\rel2\bin\srv_bare.exe"], 8096),
    ("Rust(axum)",           [r"C:\bench\rustb\target\release\zenith_bench_rust.exe"], 9108),
    ("Go(stdlib)",           [r"C:\bench\go_stdlib.exe"], 9102),
    ("Go(gin)",              [r"C:\bench\go_gin.exe"], 9103),
    ("Java(JDK-HttpServer)", ["java", "-cp", r"C:\bench", "ZenithBench"], 9101),
    ("Java(SpringBoot)",     ["java", "-jar", r"C:\bench\springb\target\bench-1.0.jar"], 9109),
    ("Node(fastify)",        ["node", r"C:\bench\node_fastify.mjs"], 9104),
    ("Node(express)",        ["node", r"C:\bench\node_express.mjs"], 9105),
    ("Node(node:http)",      ["node", r"C:\bench\node_http.mjs"], 9106),
    ("Python(FastAPI)",      ["python", r"C:\bench\py_fastapi.py"], 9107),
]

ENDPOINTS = [("/hello", "plaintext"), ("/api/users/42", "json")]

print("server,endpoint,rps_median,rps_max,p99_worst,fail,rss_mb")
for name, cmd, port in SERVERS:
    try:
        proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                creationflags=subprocess.CREATE_NEW_PROCESS_GROUP)
    except Exception as e:
        print(f"{name},START_FAIL,{e}")
        continue
    time.sleep(5)
    rss = rss_mb(proc.pid)
    for path, label in ENDPOINTS:
        url = f"http://127.0.0.1:{port}{path}"
        run_bench(url, 3)  # 预热
        rpss, fails, p99 = [], 0, 0.0
        for _ in range(ROUNDS):
            d = run_bench(url, DUR)
            if "rps" in d:
                rpss.append(float(d["rps"]))
            fails += int(d.get("requests(fail)", 0))
            if "p99" in d:
                p99 = max(p99, float(d["p99"].rstrip("s")))
        if rpss:
            rpss.sort()
            med = rpss[len(rpss) // 2]
            mx = max(rpss)
        else:
            med = mx = 0
        print(f"{name},{label},{med:.0f},{mx:.0f},{p99},{fails},{rss}")
        sys.stdout.flush()
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except Exception:
        proc.kill()
    subprocess.run(["taskkill", "/F", "/T", "/PID", str(proc.pid)],
                   capture_output=True)
    time.sleep(1.5)
