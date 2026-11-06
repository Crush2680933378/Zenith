#!/usr/bin/env python3
"""文档内链检查：README 与 docs/*.md 的相对链接与锚点是否都有效。

CI 里跑一次（linux-gcc 任务），防止重构 README / 拆分文档时留下死链。
"""

import io
import os
import re
import sys

ROOT = os.path.abspath(".")
LINK = re.compile(r"\[([^\]]+)\]\(([^)]+)\)")


def anchors_of(path):
    """收集某个 md 文件里所有标题对应的 GitHub 锚点。"""
    out = set()
    for line in io.open(path, encoding="utf-8"):
        m = re.match(r"^(#{1,6})\s+(.*)$", line.strip())
        if not m:
            continue
        text = m.group(2).strip()
        text = re.sub(r"[`*_]", "", text)
        text = text.replace(" ", "-").lower()
        text = re.sub(r"[^\w\u4e00-\u9fff\-]", "", text)
        out.add(text)
    return out


def check(md_path):
    base = os.path.dirname(md_path)
    problems = []
    text = io.open(md_path, encoding="utf-8").read()
    # 先剥掉代码块与行内代码：`[&](auto)` 这类会被误判成链接
    text = re.sub(r"```.*?```", "", text, flags=re.S)
    text = re.sub(r"`[^`]*`", "", text)
    for m in LINK.finditer(text):
        target = m.group(2).strip()
        if target.startswith(("http://", "https://", "mailto:")):
            continue
        if target.startswith("#"):
            frag = target[1:]
            if frag and frag not in anchors_of(md_path):
                problems.append((md_path, target, "同文件锚点不存在"))
            continue
        path_part, _, frag = target.partition("#")
        full = os.path.normpath(os.path.join(base, path_part))
        if not os.path.exists(full):
            problems.append((md_path, target, "文件不存在"))
        elif frag and full.endswith(".md"):
            if frag not in anchors_of(full):
                problems.append((md_path, target, "目标文件锚点不存在"))
    return problems


targets = ["README.md", "CONTRIBUTING.md", "SECURITY.md", "CHANGELOG.md"]
for d in ("docs",):
    for name in os.listdir(d):
        if name.endswith(".md"):
            targets.append(os.path.join(d, name))

all_problems = []
for t in targets:
    all_problems += check(t)

if all_problems:
    for p, target, why in all_problems:
        print(f"{p}: [{target}] -> {why}")
    sys.exit(1)
print(f"OK: {len(targets)} 个文档，内部链接全部有效")
