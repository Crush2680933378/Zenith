"""从 RFC 7541 原文（附录 B）提取 HPACK Huffman 表，生成 C++ 头文件片段。

用法：
    python tools/gen_hpack_huffman.py <rfc7541.txt> > src/http/hpack_huffman.inc
"""

import re
import sys

# 形如：   (229)  |11111111|11111111|00111    1fffe7  [21]
#      EOS (256)  |11111111|...                3fffffff  [30]
PAT = re.compile(r'\(\s*(\d+)\)\s*\|([01|]+)\s+([0-9a-fA-F]+)\s+\[\s*(\d+)\s*\]')


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: gen_hpack_huffman.py <rfc7541.txt> <out.inc>", file=sys.stderr)
        return 2
    src_path, out_path = sys.argv[1], sys.argv[2]
    text = open(src_path, encoding="utf-8", errors="replace").read()

    table = {}
    for m in PAT.finditer(text):
        sym = int(m.group(1))
        code = int(m.group(3), 16)
        bits = int(m.group(4))
        # 同一个符号在正文其它地方不会被这个格式匹配到，重复即视为异常
        if sym in table and table[sym] != (code, bits):
            print(f"conflict for symbol {sym}", file=sys.stderr)
            return 1
        table[sym] = (code, bits)

    if len(table) != 257:
        print(f"expected 257 entries, got {len(table)}", file=sys.stderr)
        return 1

    out = []
    out.append("// 由 tools/gen_hpack_huffman.py 从 RFC 7541 附录 B 生成，勿手改。")
    out.append("// 每项：{code, bits}，下标即符号（0–255 为字节值，256 为 EOS）。")
    out.append("constexpr HuffCode kHuffTable[257] = {")
    for sym in range(257):
        code, bits = table[sym]
        out.append(f"    {{0x{code:x}u, {bits}}},  // {sym}")
    out.append("};")
    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {out_path}: 257 entries")
    return 0


if __name__ == "__main__":
    sys.exit(main())
