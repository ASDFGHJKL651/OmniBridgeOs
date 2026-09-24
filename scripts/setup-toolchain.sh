#!/usr/bin/env bash
set -euo pipefail

CHECK_ONLY=0
if [[ "${1:-}" == "--check" ]]; then
    CHECK_ONLY=1
fi

if [[ -z "${MSYSTEM:-}" ]]; then
    echo "警告：当前不在 MSYS2 环境中。建议在 MSYS2 UCRT64 或 MINGW64 Shell 中运行。" >&2
fi

PREFIX="mingw-w64-ucrt-x86_64"
if [[ "${MSYSTEM:-}" == "MINGW64" ]]; then
    PREFIX="mingw-w64-x86_64"
fi

# ---------- 分组 ----------
# ★ 修复：新增 "${PREFIX}-llvm"，提供 llvm-ar（oblibc 的静态库归档必需）。
#   仅安装 clang 不会带上 llvm-ar —— 二者属于不同包。
CORE_PACKAGES=(
    git
    make
    cmake
    ninja
    "${PREFIX}-clang"
    "${PREFIX}-lld"
    "${PREFIX}-llvm"
    "${PREFIX}-binutils"
    "${PREFIX}-nasm"
    "${PREFIX}-gdb"
)

QEMU_PACKAGES=(
    "${PREFIX}-qemu"
)

PACMAN_OPTS=(
    --needed
    --noconfirm
    --disable-download-timeout
)

pacman_install_retry() {
    local label="$1"; shift
    local max_attempts=3
    local attempt=1

    while (( attempt <= max_attempts )); do
        echo ""
        echo "==> 安装 $label（第 $attempt/$max_attempts 次尝试）"
        printf '  %s\n' "$@"

        if pacman -S "${PACMAN_OPTS[@]}" "$@"; then
            echo "==> $label 安装成功"
            return 0
        fi

        echo ""
        echo "！$label 第 $attempt 次安装失败。" >&2

        if (( attempt < max_attempts )); then
            echo "  5 秒后重试（已下载的包会断点续传，不会重下）..." >&2
            sleep 5
        fi

        attempt=$(( attempt + 1 ))
    done

    echo ""
    echo "！$label 连续 $max_attempts 次安装失败。" >&2
    return 1
}

check_xfer_command() {
    if grep -q "^XferCommand" /etc/pacman.conf 2>/dev/null; then
        echo "OK: 已配置 XferCommand（curl 断点续传）"
        grep "^XferCommand" /etc/pacman.conf
    else
        echo ""
        echo "警告：未配置 XferCommand。pacman 内置下载器对不稳定网络较差。" >&2
        echo "建议执行以下命令启用 curl 断点续传：" >&2
        echo "  sed -i '/^\\[options\\]/a XferCommand = /usr/bin/curl -L -C - -f --retry 5 --retry-delay 3 --connect-timeout 30 -o %o %u' /etc/pacman.conf" >&2
        echo "" >&2
    fi
}

if [[ $CHECK_ONLY -eq 0 ]]; then
    if ! command -v pacman >/dev/null 2>&1; then
        echo "未找到 pacman。请先安装 MSYS2：https://www.msys2.org/" >&2
        exit 1
    fi

    check_xfer_command

    if grep -q "mirror.msys2.org\|repo.msys2.org" /etc/pacman.d/mirrorlist.ucrt64 2>/dev/null; then
        echo ""
        echo "警告：检测到仍在使用 MSYS2 官方镜像（国内访问极慢）。" >&2
        echo "建议切换到清华/中科大/上交镜像，参见 README。" >&2
        read -r -p "是否继续使用当前镜像？[y/N] " ans
        if [[ "${ans:-N}" != "y" && "${ans:-N}" != "Y" ]]; then
            echo "已取消。请先配置镜像。" >&2
            exit 1
        fi
    fi

    echo ""
    echo "同步 pacman 数据库..."
    pacman -Syy --noconfirm || true

    pacman_install_retry "核心工具链" "${CORE_PACKAGES[@]}" || exit 1

    pacman_install_retry "QEMU" "${QEMU_PACKAGES[@]}" || {
        echo ""
        echo "QEMU 安装失败。可稍后单独重试：" >&2
        echo "  pacman -S --needed --noconfirm --disable-download-timeout ${QEMU_PACKAGES[*]}" >&2
        echo "核心工具链已安装，可先进行 make all / make test。" >&2
        exit 1
    }
fi

check_cmd() {
    local cmd="$1"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "缺少命令：$cmd" >&2
        return 1
    fi
    echo "OK: $cmd -> $(command -v "$cmd")"
}

# ★ 修复：显式检查 llvm-ar（oblibc 静态库归档必需）
status=0
for cmd in clang lld-link llvm-ar make cmake ninja nasm gdb; do
    check_cmd "$cmd" || status=1
done

if ! command -v qemu-system-x86_64 >/dev/null 2>&1; then
    echo "警告：未找到 qemu-system-x86_64。make run 将不可用。" >&2
    echo "       单独安装：pacman -S --needed --noconfirm --disable-download-timeout ${QEMU_PACKAGES[*]}" >&2
else
    echo "OK: qemu-system-x86_64 -> $(command -v qemu-system-x86_64)"
fi

if [[ $status -ne 0 ]]; then
    echo "核心工具链检查失败。请重新运行：./scripts/setup-toolchain.sh" >&2
    exit 1
fi

OVMF_FOUND=0
OVMF_CANDIDATES=(
    "${OVMF_PATH:-}"
    "/c/Program Files/qemu/share/edk2-x86_64-code.fd"
    "/c/Program Files/qemu/share/OVMF.fd"
    "/c/Program Files/qemu/share/OVMF_CODE.fd"
    "/ucrt64/share/edk2-ovmf/x64/OVMF_CODE.fd"
    "/mingw64/share/edk2-ovmf/x64/OVMF_CODE.fd"
)

for f in "${OVMF_CANDIDATES[@]}"; do
    if [[ -n "$f" && -f "$f" ]]; then
        echo "OK: OVMF -> $f"
        OVMF_FOUND=1
        break
    fi
done

if [[ $OVMF_FOUND -eq 0 ]]; then
    echo ""
    echo "警告：未找到 OVMF 固件。MSYS2 仓库中没有 edk2-ovmf 包。" >&2
    echo "获取方式：" >&2
    echo "  1. 安装 QEMU for Windows（https://www.qemu.org/download/#windows）" >&2
    echo "  2. 从 EDK2 nightly 下载：" >&2
    echo "     curl -L -o OVMF.fd https://github.com/retrage/edk2-nightly/raw/master/bin/RELEASEX64_OVMF.fd" >&2
    echo "     export OVMF_PATH=\$(pwd)/OVMF.fd" >&2
    echo "" >&2
fi

echo ""
echo "工具链检查完成。"