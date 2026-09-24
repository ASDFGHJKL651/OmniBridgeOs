#!/usr/bin/env bash
# OmniBridgeOs/scripts/obcc.sh
# 宿主侧 .obr 编译器外壳（阶段 14a + 18C 更新）。
#
# 用法：
#   obcc.sh <input.c> -o <output.obr> [--minpriv N] [--base 0x400000]
#                                    [--entry-symbol NAME] [--no-libc]
#                                    [--dep <libname>]...
#
# 本步不调用真正的 LLVM 后端。这里只是：
#   clang (-target x86_64-pc-win32-coff)  -> lld-link -> pe_to_obr.py
#
# ★ 关键点（人工必须审查）：
#   clang 与 lld-link 是原生 Windows 程序（ucrt64 工具链），它们不认识
#   MSYS2 虚拟路径（如 /tmp/...）。因此：
#     1) 临时目录必须放在项目 build/ 下（与输出文件同一文件系统）；
#     2) 传给 clang / lld-link 的路径必须是 Windows 形式（cygpath -w）。
#     3) ★ 修复：lld-link 的参数（如 /nodefaultlib）以 '/' 开头，
#        MSYS2 会把它误当作绝对路径转换为 'D:/msys64/nodefaultlib'。
#        因此调用 lld-link 时必须设置 MSYS_NO_PATHCONV=1 禁用转换。
#
# ★ 第 18C 步更新：
#   - 默认链接 usr/lib/oblibc/liboblibc.a，除非指定 --no-libc。
#   - 若 liboblibc.a 不存在，自动调用 make -C usr/lib/oblibc。
#   - 支持 --dep <name>，可多次，用于声明 .obr 依赖的共享库。

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

INPUT=""
OUTPUT=""
MINPRIV=0
BASE=0x400000
ENTRY_SYM="_start"
USE_LIBC=1
DEPS=()      # ★ 依赖库列表（可空）

usage() {
    sed -n '2,24p' "$0"
    exit 1
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -o)               OUTPUT="$2"; shift 2 ;;
        --minpriv)        MINPRIV="$2"; shift 2 ;;
        --base)           BASE="$2"; shift 2 ;;
        --entry-symbol)   ENTRY_SYM="$2"; shift 2 ;;
        --dep)            DEPS+=( "$2" ); shift 2 ;;
        --no-libc)        USE_LIBC=0; shift ;;
        -h|--help)        usage ;;
        -*)               echo "unknown option: $1" >&2; exit 1 ;;
        *)                INPUT="$1"; shift ;;
    esac
done

if [[ -z "$INPUT" || -z "$OUTPUT" ]]; then
    usage
fi
if [[ ! -f "$INPUT" ]]; then
    echo "[obcc] input not found: $INPUT" >&2
    exit 1
fi

CLANG="${CLANG:-clang}"
LLD="${LLD_LINK:-lld-link}"

if ! command -v "$CLANG" >/dev/null 2>&1; then
    echo "[obcc] clang not found (set CLANG=)" >&2
    exit 1
fi
if ! command -v "$LLD" >/dev/null 2>&1; then
    echo "[obcc] lld-link not found (set LLD_LINK=)" >&2
    exit 1
fi

# ---- 路径转换工具 ----
to_win() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        echo "$1"
    fi
}

# ---- oblibc 库准备 ----
OBLIBC_A="$ROOT/usr/lib/oblibc/liboblibc.a"
if [[ $USE_LIBC -eq 1 && ! -f "$OBLIBC_A" ]]; then
    echo "[obcc] liboblibc.a not found, building..."
    if command -v make >/dev/null 2>&1; then
        make -C "$ROOT/usr/lib/oblibc" >/dev/null || {
            echo "[obcc] WARN: failed to build liboblibc.a, continuing without it" >&2
            USE_LIBC=0
        }
    else
        echo "[obcc] WARN: make not found, cannot build liboblibc.a" >&2
        USE_LIBC=0
    fi
fi

# ---- 临时目录：放在 build/ 下，与输出文件同一文件系统 ----
BUILD_DIR="$ROOT/build"
TMPROOT="$BUILD_DIR/obcc-tmp"
mkdir -p "$TMPROOT"
TMPDIR="$(mktemp -d "$TMPROOT/hello.XXXXXX")"
trap 'rm -rf "$TMPDIR"' EXIT

OBJ_MSYS="$TMPDIR/input.o"
PE_MSYS="$TMPDIR/input.pe"

# 确保输出目录存在
OUT_DIR_RAW="$(dirname "$OUTPUT")"
mkdir -p "$OUT_DIR_RAW"
OUT_MSYS="$(cd "$OUT_DIR_RAW" && pwd)/$(basename "$OUTPUT")"

OBJ_WIN="$(to_win "$OBJ_MSYS")"
PE_WIN="$(to_win "$PE_MSYS")"
OUT_WIN="$(to_win "$OUT_MSYS")"
INPUT_WIN="$(to_win "$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")")"

echo "[obcc] $(basename "$INPUT") -> $OBJ_MSYS"

# ---- 1) clang：源码 -> COFF 目标文件 ----
"$CLANG" \
    -target x86_64-pc-win32-coff \
    -ffreestanding -fno-stack-protector -fno-builtin \
    -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
    -fno-pic -fno-asynchronous-unwind-tables -fno-unwind-tables \
    -mcmodel=small \
    -I "$ROOT/usr/include" \
    -O2 -Wall -Wextra \
    -c "$INPUT_WIN" -o "$OBJ_WIN"

echo "[obcc] lld-link -> $PE_MSYS"

# ---- 2) lld-link：COFF 目标文件 -> PE32+ ----
# ★ 关键：MSYS_NO_PATHCONV=1 禁用 MSYS2 的参数路径转换。
LLD_ARGS=(
    /subsystem:native
    "/entry:${ENTRY_SYM}"
    /nodefaultlib
    /machine:x64
    "/base:${BASE}"
    "/out:${PE_WIN}"
    "$OBJ_WIN"
)

if [[ $USE_LIBC -eq 1 && -f "$OBLIBC_A" ]]; then
    LLD_ARGS+=( "$(to_win "$OBLIBC_A")" )
fi

MSYS_NO_PATHCONV=1 "$LLD" "${LLD_ARGS[@]}"

# ---- 3) pe_to_obr.py：PE32+ -> .obr ----
mkdir -p "$(dirname "$OUT_MSYS")"
SCRIPT_WIN="$(to_win "$SCRIPT_DIR/pe_to_obr.py")"

# ★ 关键修复：空数组必须用 "${arr[@]}" 展开为 0 个参数，
#   绝不能用 "${arr[@]:-}"（会展开为 1 个空字符串）。
DEP_ARGS=()
for d in "${DEPS[@]:-}"; do
    [[ -n "$d" ]] && DEP_ARGS+=( --dep "$d" )
done

if [[ ${#DEP_ARGS[@]} -gt 0 ]]; then
    python3 "$SCRIPT_WIN" "$PE_WIN" "$OUT_WIN" \
            --minpriv "$MINPRIV" "${DEP_ARGS[@]}"
else
    python3 "$SCRIPT_WIN" "$PE_WIN" "$OUT_WIN" \
            --minpriv "$MINPRIV"
fi

echo "[obcc] done: $OUTPUT"