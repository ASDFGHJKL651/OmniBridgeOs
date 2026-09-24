#!/usr/bin/env python3
# OmniBridgeOs/scripts/gen_ita_hashes.py
"""
ITA 第一层固化哈希生成工具。

用法：
    python3 scripts/gen_ita_hashes.py <critical_dir> <out.h>

行为：
    - 扫描 <critical_dir> 下的 5 个核心 .obr 文件（init/secmgr/
      servicehost/auditd/kernel_manager）。
    - 对每个文件计算 SHA-384。
    - 生成 kernel/arch/x64/ita_fixed_hashes.h。
    - 若某文件不存在，则使用 SHA-384("") 占位并打印警告。

**人工必须审查**：本工具生成的哈希一旦被编译进内核 .rodata，即为
最终固化值，无法通过运行时配置修改。
"""

import hashlib
import os
import sys
import textwrap

TARGETS = [
    "init.obr",
    "secmgr.obr",
    "servicehost.obr",
    "auditd.obr",
    "kernel_manager.obr",
]

PREFIX = "/system/critical/"

# SHA-384("") 作为占位（已核验与 FIPS 180-4 一致）
PLACEHOLDER = bytes.fromhex(
    "38b060a751ac96384cd9327eb1b1e36a21fdb71114be07434c0cc7bf63f6e1da"
    "274edebfe76f65fbd51ad2f14898b95b"
)
assert len(PLACEHOLDER) == 48


def fmt_hash(h: bytes) -> str:
    lines = []
    for i in range(0, 48, 8):
        chunk = ", ".join(f"0x{b:02x}" for b in h[i:i+8])
        lines.append(f"            {chunk},")
    return "\n".join(lines)


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)

    crit_dir = sys.argv[1]
    out_path = sys.argv[2]

    entries = []
    any_missing = False

    for name in TARGETS:
        full = os.path.join(crit_dir, name)
        if not os.path.isfile(full):
            print(f"[WARN] {full} not found -> placeholder hash", file=sys.stderr)
            h = PLACEHOLDER
            any_missing = True
        else:
            data = open(full, "rb").read()
            h = hashlib.sha384(data).digest()
            print(f"[OK]   {full}: {h.hex()}")
        entries.append((PREFIX + name, h))

    with open(out_path, "w") as f:
        f.write("/*===OmniBridgeOs/kernel/arch/x64/ita_fixed_hashes.h===*/\n")
        f.write("/*\n")
        f.write(" * ITA 第一层固化哈希白名单 —— 由 scripts/gen_ita_hashes.py 自动生成。\n")
        f.write(" * 请勿手工修改；重新生成请运行脚本。\n")
        f.write(" */\n")
        f.write("#ifndef OMNIBRIDGE_ITA_FIXED_HASHES_H\n")
        f.write("#define OMNIBRIDGE_ITA_FIXED_HASHES_H\n\n")
        f.write("#include \"ita.h\"\n\n")
        f.write("static const struct ita_fixed_entry g_ita_fixed_whitelist[] = {\n")

        for path, h in entries:
            f.write("    {\n")
            f.write(f'        "{path}",\n')
            f.write("        {\n")
            f.write(fmt_hash(h))
            f.write("\n        }\n")
            f.write("    },\n")
        f.write("};\n\n")

        f.write(
            "static const uint32_t g_ita_fixed_count =\n"
            "    (uint32_t)(sizeof(g_ita_fixed_whitelist) /\n"
            "               sizeof(g_ita_fixed_whitelist[0]));\n\n"
        )
        f.write("#endif /* OMNIBRIDGE_ITA_FIXED_HASHES_H */\n")

    print(f"[gen_ita_hashes] wrote {out_path} ({len(entries)} entries)")
    if any_missing:
        print("[gen_ita_hashes] WARNING: some files missing, placeholder hashes used")


if __name__ == "__main__":
    main()