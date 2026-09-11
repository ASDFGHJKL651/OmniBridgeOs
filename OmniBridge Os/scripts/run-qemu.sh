#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
ESP="$BUILD/esp"
LOG="$BUILD/serial.log"
mkdir -p "$BUILD"

# ---- 路径转 Windows 形式（QEMU 是原生 Windows 程序）----
to_win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

QEMU="${QEMU_BIN:-qemu-system-x86_64}"
if ! command -v "$QEMU" >/dev/null 2>&1; then
    echo "未找到 qemu-system-x86_64" >&2
    exit 1
fi

OVMF="${OVMF_PATH:-}"
if [[ -z "${OVMF:-}" || ! -f "$OVMF" ]]; then
    for c in \
        "/d/OmniBridgeOs/OVMF.fd" \
        "/usr/share/OVMF/OVMF_CODE.fd" \
        "/usr/share/OVMF/OVMF_CODE_4M.fd" \
        "/usr/share/ovmf/OVMF.fd" \
        "/usr/share/edk2-ovmf/x64/OVMF_CODE.fd" \
        "/ucrt64/share/edk2-ovmf/x64/OVMF_CODE.fd" \
        "/mingw64/share/edk2-ovmf/x64/OVMF_CODE.fd" ; do
        if [[ -f "$c" ]]; then OVMF="$c"; break; fi
    done
fi
if [[ -z "${OVMF:-}" ]]; then
    echo "未找到 OVMF，设置 OVMF_PATH" >&2
    exit 1
fi

# 尝试找 VARS
OVMF_VARS=""
case "$OVMF" in
  *OVMF_CODE*)
    cand1="$(dirname "$OVMF")/OVMF_VARS.fd"
    cand2="$(dirname "$OVMF")/edk2-x86_64-vars.fd"
    [[ -f "$cand1" ]] && OVMF_VARS="$cand1"
    [[ -z "$OVMF_VARS" && -f "$cand2" ]] && OVMF_VARS="$cand2"
    if [[ -z "$OVMF_VARS" ]]; then
        echo "找到 CODE 但找不到 VARS" >&2; exit 1
    fi
    cp "$OVMF_VARS" "$BUILD/OVMF_VARS.fd"
    OVMF_VARS="$BUILD/OVMF_VARS.fd"
    ;;
esac

# ---- 转换成 Windows 路径 ----
ESP_WIN="$(to_win "$ESP")"
OVMF_WIN="$(to_win "$OVMF")"
OVMF_VARS_WIN=""
[[ -n "$OVMF_VARS" ]] && OVMF_VARS_WIN="$(to_win "$OVMF_VARS")"

ARGS=(
  -machine q35 -m 512 -accel tcg -display none -no-reboot
  -device isa-debug-exit,iobase=0x501,iosize=0x04
  -drive "file=fat:rw:$ESP_WIN,format=raw"
  -serial stdio -monitor none
)

if [[ -n "$OVMF_VARS_WIN" ]]; then
    ARGS+=(-drive "if=pflash,format=raw,readonly=on,file=$OVMF_WIN")
    ARGS+=(-drive "if=pflash,format=raw,file=$OVMF_VARS_WIN")
else
    ARGS+=(-bios "$OVMF_WIN")
fi

echo "启动 QEMU..."
echo "  ESP (msys)  : $ESP"
echo "  ESP (win)   : $ESP_WIN"
echo "  OVMF (win)  : $OVMF_WIN"
echo "  串口日志    : $LOG"

set +e
"$QEMU" "${ARGS[@]}" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e
echo "QEMU 退出码: $rc (isa-debug-exit: (n<<1)|1)"