#!/usr/bin/env python3
# OmniBridgeOs/scripts/merge_sources.py
"""
merge_sources.py —— 将 OmniBridge OS 的源码按类别合并为单一文件，
便于整体上传给 AI。

用法：
    python3 merge_sources.py                    # 自动探测项目根
    python3 merge_sources.py -r D:\\OmniBridgeOs # 显式指定
    python3 merge_sources.py -o D:\\0            # 显式指定输出目录

自动探测项目根的顺序：
    1. 命令行 -r/--root
    2. cwd 下若存在 OmniBridgeOs/ 子目录，且其中含标记子目录，则用它
    3. 从 cwd 向上逐级查找含 kernel/arch/x64 或 boot/uefi 的目录
    4. 脚本目录下若存在 OmniBridgeOs/ 子目录，且其中含标记，则用它
    5. 从脚本目录向上查找同样标记
    6. 全部失败 -> 报错并提示用 -r

生成的文件（默认位于 <root>/0 下）：

  /*...*/ 注释风格（.c / .h / .S 类）：
    C source codes of path kernel_arch_x64.c
    C source codes of path kernel_arch_x64_net.c
    C source codes of path kernel_arch_x64_block.c
    C source codes of path boot_uefi.c
    headers of path kernel_arch_x64.h
    headers of path kernel_arch_x64_net.h
    headers of path kernel_arch_x64_block.h
    headers of path kernel_arch_x64_user.h
    headers of path boot_uefi.h
    Assembler source of path boot_uefi.S
    Assembler source of path kernel_arch_x64.S
    Assembler source of path usr_lib_oblibc.S        ★ 新增
    C source codes of path tests_host.c
    C source codes of path usr_lib_oblibc.c           ★ 新增
    C source codes of path usr_examples.c             ★ 新增
    headers of path usr_include_ob.h                  ★ 新增
    headers of path usr_include_ob_sys.h              ★ 新增

  # 注释风格（.py / .ps1 / .sh 类）：
    python source codes of path scripts.py
    powershell source codes of path scripts.ps1
    SH source codes of path scripts.sh

  普通复制（Makefile 类）：
    makefile of omnibridgeOs' root path.txt
    makefile of path tests_host.txt
    makefile of path usr_lib_oblibc.txt               ★ 新增

合并文件规则（.c/.h/.S/.py/.ps1/.sh 类）：
    - 目标文件的前 6 行（若存在）保留不动，从第 7 行开始重写；
    - 若目标文件不存在，则使用脚本内置的默认 6 行头部；
    - 每个源文件按以下格式写入（c_style 决定分隔符）：
        c_style=True :  /*===OmniBridgeOs/<相对路径>===*/  ...  /*===... 结束===*/
        c_style=False:  #===OmniBridgeOs/<相对路径>===      ...  #===... 结束===
        之后追加一个空行；
    - 源文件按文件名升序排列；行尾统一为 LF。

普通复制规则（Makefile 类）：
    - 目标文件已存在则保留其第 1 行，从源文件第 2 行起写入其余内容；
    - 目标文件不存在则直接写入源文件全文；
    - 无其他包装；行尾统一为 LF。
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

OMNI_PREFIX = "OmniBridgeOs"
HEADER_LINES = 6

# 合并任务：
#   (源目录相对项目根的路径, 扩展名, 输出文件名, c_style)
#   c_style=True  -> /*...*/ 分隔符
#   c_style=False -> #       分隔符
MERGE_JOBS: list[tuple[str, str, str, bool]] = [
    # -------- 内核：C 源码 --------
    ("kernel/arch/x64", ".c", "C source codes of path kernel_arch_x64.c", True),
    ("kernel/arch/x64/net", ".c",
        "C source codes of path kernel_arch_x64_net.c", True),
    ("kernel/arch/x64/block", ".c",
        "C source codes of path kernel_arch_x64_block.c", True),
    ("kernel/arch/x64/user", ".c",
        "C source codes of path kernel_arch_x64_user.c", True),

    # -------- 内核：头文件 --------
    ("kernel/arch/x64", ".h", "headers of path kernel_arch_x64.h", True),
    ("kernel/arch/x64/net", ".h",
        "headers of path kernel_arch_x64_net.h", True),
    ("kernel/arch/x64/block", ".h",
        "headers of path kernel_arch_x64_block.h", True),
    ("kernel/arch/x64/user", ".h",
        "headers of path kernel_arch_x64_user.h", True),

    # -------- 内核：汇编 --------
    ("kernel/arch/x64", ".S", "Assembler source of path kernel_arch_x64.S", True),
    ("kernel/arch/x64/user", ".S",
        "Assembler source of path kernel_arch_x64_user.S", True),

    # -------- UEFI 引导 --------
    ("boot/uefi", ".c", "C source codes of path boot_uefi.c", True),
    ("boot/uefi", ".h", "headers of path boot_uefi.h", True),
    ("boot/uefi", ".S", "Assembler source of path boot_uefi.S", True),

    # -------- 宿主测试 --------
    ("tests/host", ".c", "C source codes of path tests_host.c", True),

    # -------- ★ 用户态：oblibc 与示例 --------
    # 头文件拆两个目录：usr/include/ob 与 usr/include/ob/sys
    ("usr/include/ob",     ".h", "headers of path usr_include_ob.h",     True),
    ("usr/include/ob/sys", ".h", "headers of path usr_include_ob_sys.h", True),
    # oblibc 的 C 源码与 crt0.S
    ("usr/lib/oblibc", ".c", "C source codes of path usr_lib_oblibc.c",   True),
    ("usr/lib/oblibc", ".S", "Assembler source of path usr_lib_oblibc.S", True),
    # 用户态示例
    ("usr/examples",   ".c", "C source codes of path usr_examples.c",     True),

    # -------- 脚本 --------
    ("scripts", ".py",  "python source codes of path scripts.py",      False),
    ("scripts", ".ps1", "powershell source codes of path scripts.ps1", False),
    ("scripts", ".sh",  "SH source codes of path scripts.sh",          False),
]

# 普通复制任务：(源文件相对项目根的路径, 输出文件名)
COPY_JOBS: list[tuple[str, str]] = [
    ("Makefile",                "makefile of omnibridgeOs' root path.txt"),
    ("tests/host/Makefile",     "makefile of path tests_host.txt"),
    # ★ 新增：用户态库 Makefile
    ("usr/lib/oblibc/Makefile", "makefile of path usr_lib_oblibc.txt"),
]

# 用于识别项目根的"标记"子目录（任一存在即认为是项目根）
MARKERS: tuple[str, ...] = (
    "kernel/arch/x64",
    "boot/uefi",
    # ★ 新增：把 usr/include/ob 也作为标记，使"只上传 usr/"的镜像
    #   也能被正确定位
    "usr/include/ob",
)

PREFERRED_SUBDIR_NAME = "OmniBridgeOs"


# ---------------------------------------------------------------------------
# 项目根探测
# ---------------------------------------------------------------------------

def _has_marker(d: Path) -> bool:
    return any((d / m).is_dir() for m in MARKERS)


def _walk_up_with_marker(start: Path) -> Path | None:
    cur = start.resolve()
    while True:
        if _has_marker(cur):
            return cur
        parent = cur.parent
        if parent == cur:
            return None
        cur = parent


def find_root(explicit: str | None) -> Path | None:
    if explicit:
        p = Path(explicit).expanduser().resolve()
        return p if p.is_dir() else None

    cwd = Path.cwd().resolve()
    script_dir = Path(__file__).resolve().parent

    cand = cwd / PREFERRED_SUBDIR_NAME
    if cand.is_dir() and _has_marker(cand):
        return cand.resolve()

    up = _walk_up_with_marker(cwd)
    if up is not None:
        return up

    cand2 = script_dir / PREFERRED_SUBDIR_NAME
    if cand2.is_dir() and _has_marker(cand2):
        return cand2.resolve()

    up2 = _walk_up_with_marker(script_dir)
    if up2 is not None:
        return up2

    return None


# ---------------------------------------------------------------------------
# 头部 / IO
# ---------------------------------------------------------------------------

def default_header(rel_dir: str, ext: str, c_style: bool) -> str:
    """生成默认的 6 行头部（含末尾空行）。c_style 决定注释风格。"""
    body_lines = [
        f"目录 {OMNI_PREFIX}/{rel_dir}/ 下的所有{ext}文件",
        "该文件用于上传给AI以避免上传文件数量过多",
        "实际输出代码时应按文件逐个给出",
    ]
    if c_style:
        return "/*\n" + "\n".join(body_lines) + "\n*/\n\n"
    else:
        return "\n".join("#" + ln if ln else "#" for ln in body_lines) + "\n\n"


def read_existing_header(path: Path) -> str | None:
    """读取目标文件的前 HEADER_LINES 行（统一为 LF）；不足则返回 None。"""
    if not path.is_file():
        return None
    try:
        with path.open("r", encoding="utf-8", newline="") as f:
            lines: list[str] = []
            for _ in range(HEADER_LINES):
                line = f.readline()
                if line == "":
                    return None
                lines.append(line)
    except (OSError, UnicodeDecodeError):
        return None
    return "".join(
        ln.replace("\r\n", "\n").replace("\r", "\n") for ln in lines
    )


def read_first_line(path: Path) -> str | None:
    """读取目标文件第 1 行（统一为 LF）；文件不存在或为空则返回 None。"""
    if not path.is_file():
        return None
    try:
        with path.open("r", encoding="utf-8", newline="") as f:
            line = f.readline()
    except (OSError, UnicodeDecodeError):
        return None
    if line == "":
        return None
    line = line.replace("\r\n", "\n").replace("\r", "\n")
    if not line.endswith("\n"):
        line += "\n"
    return line


def read_source_text(path: Path) -> str:
    """按文本读取源文件，遇非 UTF-8 时用 'replace' 兜底。"""
    try:
        text = path.read_text(encoding="utf-8")
    except UnicodeDecodeError:
        text = path.read_text(encoding="utf-8", errors="replace")
    except OSError as e:
        raise RuntimeError(f"无法读取 {path}: {e}") from e
    return text.replace("\r\n", "\n").replace("\r", "\n")


def read_source_lines(path: Path) -> list[str]:
    """读取源文件为行列表（保留换行符，统一 LF）。"""
    text = read_source_text(path)
    lines = text.splitlines(keepends=True)
    if lines and not lines[-1].endswith("\n"):
        lines[-1] += "\n"
    return lines


def write_text_lf(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(text)


# ---------------------------------------------------------------------------
# 合并（.c/.h/.S/.py/.ps1/.sh）
# ---------------------------------------------------------------------------

def _begin_marker(rel_posix: str, c_style: bool) -> str:
    if c_style:
        return f"/*==={rel_posix}===*/\n"
    return f"#==={rel_posix}===\n"


def _end_marker(rel_posix: str, c_style: bool) -> str:
    if c_style:
        return f"/*==={rel_posix} 结束===*/\n"
    return f"#==={rel_posix} 结束===\n"


def merge_one(root: Path, rel_dir: str, ext: str, out_path: Path,
              c_style: bool) -> int:
    src_dir = root / rel_dir
    if not src_dir.is_dir():
        print(f"[skip] 源目录不存在: {src_dir}", file=sys.stderr)
        return 0

    files = sorted(p for p in src_dir.glob(f"*{ext}") if p.is_file())

    existing = read_existing_header(out_path)
    header = existing if existing is not None \
             else default_header(rel_dir, ext, c_style)

    chunks: list[str] = [header]

    for src in files:
        rel_posix = f"{OMNI_PREFIX}/{(Path(rel_dir) / src.name).as_posix()}"
        body = read_source_text(src)
        if not body.endswith("\n"):
            body += "\n"

        chunks.append(_begin_marker(rel_posix, c_style))
        chunks.append(body)
        chunks.append(_end_marker(rel_posix, c_style))
        chunks.append("\n")

    write_text_lf(out_path, "".join(chunks))
    style = "/*..*/" if c_style else "#"
    print(f"[ok] {out_path}  <-  {len(files)} 个 {ext} 文件  "
          f"({style})  (from {src_dir})")
    return len(files)


# ---------------------------------------------------------------------------
# 普通复制（Makefile 类）
# ---------------------------------------------------------------------------

def copy_plain(root: Path, rel_src: str, out_path: Path) -> bool:
    src = root / rel_src
    if not src.is_file():
        print(f"[skip] 源文件不存在: {src}", file=sys.stderr)
        return False

    src_lines = read_source_lines(src)
    keep_line = read_first_line(out_path)

    if keep_line is not None and src_lines:
        content = keep_line + "".join(src_lines[1:])
        preserved = True
    else:
        content = "".join(src_lines)
        preserved = False

    write_text_lf(out_path, content)
    print(f"[ok] {out_path}  <-  {src}  (preserve line1: {preserved})")
    return True


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

def main() -> int:
    ap = argparse.ArgumentParser(
        description="将 OmniBridge OS 源码按类别合并为单一文件，便于上传给 AI。"
    )
    ap.add_argument("-r", "--root", default=None,
                    help="项目根目录（默认：自动探测 OmniBridgeOs 目录）")
    ap.add_argument("-o", "--out", default=None,
                    help="输出目录（默认：<root>/0）")
    args = ap.parse_args()

    root = find_root(args.root)
    if root is None:
        print(
            "错误：未能自动定位项目根。请用 -r 显式指定，例如：\n"
            "    python3 merge_sources.py -r D:\\OmniBridgeOs",
            file=sys.stderr,
        )
        return 1

    print(f"[info] 项目根 = {root}")

    out_dir = Path(args.out).expanduser().resolve() if args.out else (root / "0")
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[info] 输出目录 = {out_dir}")

    total_files = 0
    for rel_dir, ext, out_name, c_style in MERGE_JOBS:
        total_files += merge_one(root, rel_dir, ext, out_dir / out_name, c_style)

    copied = 0
    for rel_src, out_name in COPY_JOBS:
        if copy_plain(root, rel_src, out_dir / out_name):
            copied += 1

    print(f"[done] 合并 {total_files} 个源文件；普通复制 {copied} 个文件；"
          f"输出目录 = {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())