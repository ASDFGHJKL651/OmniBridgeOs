#!/usr/bin/env bash
#===OmniBridgeOs/scripts/setup-toolchain.sh===
#
# OmniBridge OS 工具链安装脚本。
#
# 人工必须审查：
#   - 本脚本在 MSYS2 环境下运行（UCRT64 / MINGW64 Shell）。
#   - 主工具链（clang/lld/llvm-ar/qemu/...）为**必装**；
#     安装失败会中止脚本。
#   - Linux ELF64 交叉工具链（x86_64-linux-gnu-gcc 或 musl-gcc）为
#     **可选**；安装失败只打印 WARN 并给出 Docker/WSL 备用方案。
#     Makefile 中 linux_hello.elf 规则会因编译器缺失而跳过，
#     内核本身仍能构建（第 19 步会因此少一个端到端程序）。
#   - 本脚本不下载任意 URL 之外的网络内容；所有包名均由
#     pacman 从已配置的镜像源获取。
#
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

# ============================================================
# ★ 第 19 步：Linux ELF64 交叉工具链候选包
#
# 人工必须审查：
#   - MSYS2 官方仓库对 Linux 交叉 gcc 的支持不稳定；不同镜像/
#     不同时期提供的包名可能不同。这里按优先级罗列常见候选。
#   - 任一候选安装成功即停止；全部失败仅 WARN。
#   - 若用户在 MSYS 环境中已有 musl-gcc（如通过 `pacman -S
#     mingw-w64-ucrt-x86_64-musl`），则视为满足条件（makefile
#     已支持 musl-gcc 回退）。
# ============================================================
LINUX_CROSS_CANDIDATE_PACKAGES=(
    # 最可能的正式包名（ucrt64 交叉工具链的命名约定）
    "${PREFIX}-x86_64-linux-gnu-gcc"
    # 部分镜像使用的变体名
    "${PREFIX}-cross-gcc-linux"
    # 纯用户态 musl 交叉（不同镜像下可能作为独立包）
    "${PREFIX}-musl"
)

PACMAN_OPTS=(
    --needed
    --noconfirm
    --disable-download-timeout
)

# ------------------------------------------------------------
# 必装包安装（重试 3 次）
# ------------------------------------------------------------
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

# ------------------------------------------------------------
# 可选包安装（单次尝试，不重试，失败不阻塞）
#
# 人工必须审查：
#   - 用于"不确定是否存在"的包；例如跨平台工具链的别名。
#   - 使用 --needed 避免重复安装。
#   - 失败时返回非零，但调用者应吞掉返回值。
# ------------------------------------------------------------
pacman_install_optional() {
    local pkg="$1"
    # 先检查是否已安装（避免每次下载索引）
    if pacman -Qi "$pkg" >/dev/null 2>&1; then
        echo "==> 可选包 $pkg 已安装"
        return 0
    fi
    echo "==> 尝试安装可选包 $pkg ..."
    if pacman -S "${PACMAN_OPTS[@]}" "$pkg" >/dev/null 2>&1; then
        echo "==> 可选包 $pkg 安装成功"
        return 0
    fi
    echo "！可选包 $pkg 不在当前镜像源中" >&2
    return 1
}

# ------------------------------------------------------------
# 下载器配置检查
# ------------------------------------------------------------
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

# ============================================================
# ★ 第 19 步：Linux 交叉 gcc 安装
#
# 语义：
#   1) 若已存在 x86_64-linux-gnu-gcc 或 musl-gcc，直接返回 0。
#   2) 否则遍历 LINUX_CROSS_CANDIDATE_PACKAGES，第一个安装成功的
#      候选胜出。
#   3) 全部失败：打印三种备用方案（Docker / WSL / 预编译下载），
#      返回值 1（调用者可视作非致命）。
#
# 人工必须审查：
#   - 备用方案只是"打印"，不自动执行任何下载/容器命令。
#   - 每个候选包的安装都是 pacman 从已配置镜像源拉取，不涉及
#     第三方 URL。
# ============================================================
install_linux_cross_gcc() {
    # 已存在？
    if command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
        echo "OK: x86_64-linux-gnu-gcc 已存在 -> $(command -v x86_64-linux-gnu-gcc)"
        return 0
    fi
    if command -v musl-gcc >/dev/null 2>&1; then
        echo "OK: musl-gcc 已存在 -> $(command -v musl-gcc)"
        return 0
    fi

    echo ""
    echo "==> 尝试安装 Linux 交叉工具链..."

    local cand
    for cand in "${LINUX_CROSS_CANDIDATE_PACKAGES[@]}"; do
        if pacman_install_optional "$cand"; then
            # 装完立刻复检命令是否存在（防止包安装成功但命令名不符）
            if command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
                echo "OK: x86_64-linux-gnu-gcc -> $(command -v x86_64-linux-gnu-gcc)"
                return 0
            fi
            if command -v musl-gcc >/dev/null 2>&1; then
                echo "OK: musl-gcc -> $(command -v musl-gcc)"
                return 0
            fi
            echo "！包 $cand 安装成功，但未提供 x86_64-linux-gnu-gcc / musl-gcc" >&2
        fi
    done

    # 全部失败
    echo ""
    echo "================================================================" >&2
    echo "警告：未能通过 pacman 安装 Linux ELF64 交叉工具链。" >&2
    echo "      Linux 端到端验收 (linux_hello.elf) 将被跳过；" >&2
    echo "      内核与其余测试不受影响。" >&2
    echo "----------------------------------------------------------------" >&2
    echo "备用方案（三选一）：" >&2
    echo "" >&2
    echo "  [A] Docker（推荐，无需污染宿主环境）" >&2
    echo "      docker run --rm -v \"\$PWD\":/w -w /w ubuntu:22.04 bash -c \\" >&2
    echo "        \"apt update && apt install -y gcc && \\" >&2
    echo "         gcc -static -O2 -s -o build/esp/bin/linux_hello.elf \\" >&2
    echo "             usr/examples/linux_hello.c\"" >&2
    echo "" >&2
    echo "  [B] WSL2（若已启用）" >&2
    echo "      wsl -- bash -c 'sudo apt update && sudo apt install -y gcc && \\" >&2
    echo "         gcc -static -O2 -s -o /mnt/d/omnibridgeos/build/esp/bin/linux_hello.elf \\" >&2
    echo "             /mnt/d/omnibridgeos/usr/examples/linux_hello.c'" >&2
    echo "" >&2
    echo "  [C] 手动下载 musl 交叉工具链（Linux x86_64 宿主机）" >&2
    echo "      wget https://musl.cc/x86_64-linux-musl-cross.tgz" >&2
    echo "      tar xzf x86_64-linux-musl-cross.tgz" >&2
    echo "      export PATH=\$PWD/x86_64-linux-musl-cross/bin:\$PATH" >&2
    echo "      # 然后在 build 时使用 MUSL_CC=x86_64-linux-musl-gcc make all" >&2
    echo "================================================================" >&2
    return 1
}

# ------------------------------------------------------------
# 主流程
# ------------------------------------------------------------
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

    # ★ 第 19 步：尝试安装 Linux 交叉工具链（非致命）
    install_linux_cross_gcc || true
fi

# ------------------------------------------------------------
# 检查
# ------------------------------------------------------------
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

# ★ 第 19 步：Linux 交叉工具链检查（非致命）
if command -v x86_64-linux-gnu-gcc >/dev/null 2>&1; then
    echo "OK: x86_64-linux-gnu-gcc -> $(command -v x86_64-linux-gnu-gcc)"
elif command -v musl-gcc >/dev/null 2>&1; then
    echo "OK: musl-gcc -> $(command -v musl-gcc)"
else
    echo "警告：未找到 x86_64-linux-gnu-gcc / musl-gcc。" >&2
    echo "      linux_hello.elf 将不会被构建；" >&2
    echo "      若需要 Linux ELF64 端到端验收，请参考上文 [A]/[B]/[C] 备用方案。" >&2
fi

if [[ $status -ne 0 ]]; then
    echo "核心工具链检查失败。请重新运行：./scripts/setup-toolchain.sh" >&2
    exit 1
fi

# ------------------------------------------------------------
# OVMF 检查
# ------------------------------------------------------------
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
#===OmniBridgeOs/scripts/setup-toolchain.sh 结束===