#===OmniBridgeOs/scripts/run-qemu.sh===
#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build"
ESP="$BUILD/esp"
LOG="$BUILD/serial.log"
OBFS_IMG="$BUILD/obfs.img"
mkdir -p "$BUILD"

to_win() { cygpath -w "$1" 2>/dev/null || echo "$1"; }

# ---------- ★ 第 18B 步：确保 OBFS 镜像存在 ----------
# 若镜像不存在，创建一个 64MB 的空白镜像。
# 首次启动时 OBFS-RW mount 会失败，内核回退到 tmpfs 根；
# 后续步骤引入 mkfs.obfs 工具后再由此脚本生成有效镜像。
if [[ ! -f "$OBFS_IMG" ]]; then
    echo "创建空白 OBFS 镜像: $OBFS_IMG (64 MiB)"
    if command -v dd >/dev/null 2>&1; then
        dd if=/dev/zero of="$OBFS_IMG" bs=1M count=64 status=none
    elif command -v truncate >/dev/null 2>&1; then
        truncate -s 64M "$OBFS_IMG"
    else
        # 最后的回退：用 Python
        python3 -c "import sys; f=open(sys.argv[1],'wb'); f.truncate(64*1024*1024); f.close()" "$OBFS_IMG"
    fi
fi

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

ESP_WIN="$(to_win "$ESP")"
OVMF_WIN="$(to_win "$OVMF")"
OBFS_WIN="$(to_win "$OBFS_IMG")"
OVMF_VARS_WIN=""
[[ -n "$OVMF_VARS" ]] && OVMF_VARS_WIN="$(to_win "$OVMF_VARS")"

# ★ 第 18A 步：增加 VirtIO 网卡
# ★ 第 18B 步：增加 VirtIO 块设备（vda，64 MiB）
ARGS=(
  -machine q35 -cpu max -m 512 -accel tcg -display none -no-reboot
  -device isa-debug-exit,iobase=0x501,iosize=0x04
  -netdev user,id=n0 -device virtio-net-pci,netdev=n0,mac=52:54:00:12:34:56
  -drive "file=$OBFS_WIN,if=none,id=hd0,format=raw"
  -device "virtio-blk-pci,drive=hd0"
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
echo "  OBFS 镜像   : $OBFS_IMG"
echo "  串口日志    : $LOG"
echo "  网络        : user-mode NAT (10.0.2.0/24), eth0=virtio-net-pci"
echo "  存储        : vda=virtio-blk-pci (64 MiB)"

set +e
"$QEMU" "${ARGS[@]}" 2>&1 | tee "$LOG"
rc=${PIPESTATUS[0]}
set -e
echo "QEMU 退出码: $rc (isa-debug-exit: (n<<1)|1)"
#===OmniBridgeOs/scripts/run-qemu.sh 结束===