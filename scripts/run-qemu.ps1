#===OmniBridgeOs/scripts/run-qemu.ps1===
$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $ProjectRoot "build"
$EspDir = Join-Path $BuildDir "esp"
$SerialLog = Join-Path $BuildDir "serial.log"
$ObfsImg = Join-Path $BuildDir "obfs.img"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# ---------- ★ 第 18B 步：确保 OBFS 镜像存在 ----------
# 若镜像不存在，创建一个 64MB 的空白镜像。
# 首次启动时 OBFS-RW mount 会失败，内核回退到 tmpfs 根；
# 后续步骤引入 mkfs.obfs 工具后再由此脚本生成有效镜像。
if (-not (Test-Path $ObfsImg)) {
    Write-Host "创建空白 OBFS 镜像: $ObfsImg (64 MiB)"
    $fs = [System.IO.File]::Create($ObfsImg)
    $fs.SetLength(64 * 1024 * 1024)
    $fs.Close()
}

$Qemu = $env:QEMU_BIN
if (-not $Qemu) {
    $cmd = Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue
    if (-not $cmd) {
        $cmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    }
    if (-not $cmd) {
        throw "未找到 qemu-system-x86_64，请安装 QEMU 或设置 QEMU_BIN"
    }
    $Qemu = $cmd.Source
}

$Ovmf = $env:OVMF_PATH
if (-not $Ovmf -or -not (Test-Path $Ovmf)) {
    $candidates = @(
        "C:\Program Files\qemu\share\edk2-x86_64-code.fd",
        "C:\Program Files\qemu\share\OVMF.fd",
        "C:\Program Files\qemu\share\OVMF_CODE.fd",
        "C:\msys64\ucrt64\share\edk2-ovmf\x64\OVMF_CODE.fd",
        "C:\msys64\mingw64\share\edk2-ovmf\x64\OVMF_CODE.fd",
        "C:\msys64\ucrt64\share\edk2-ovmf\x64\OVMF.fd",
        "C:\msys64\mingw64\share\edk2-ovmf\x64\OVMF.fd"
    )
    foreach ($c in $candidates) {
        if (Test-Path $c) { $Ovmf = $c; break }
    }
}
if (-not $Ovmf) {
    throw "未找到 OVMF，请设置 OVMF_PATH 或安装 edk2-ovmf"
}

$OvmfCode = $null
$OvmfVars = $null
$BuildVars = $null

if ($Ovmf -match "OVMF_CODE\.fd$" -or $Ovmf -match "edk2-x86_64-code\.fd$") {
    $OvmfCode = $Ovmf
    $OvmfDir = Split-Path -Parent $Ovmf
    $varsCandidates = @(
        (Join-Path $OvmfDir "OVMF_VARS.fd"),
        (Join-Path $OvmfDir "edk2-x86_64-vars.fd")
    )
    foreach ($v in $varsCandidates) {
        if (Test-Path $v) { $OvmfVars = $v; break }
    }
    if (-not $OvmfVars) { throw "找到 OVMF CODE 但未找到 VARS" }
    $BuildVars = Join-Path $BuildDir "OVMF_VARS.fd"
    Copy-Item $OvmfVars $BuildVars -Force
}

# ★ 第 18A 步：增加 VirtIO 网卡，使 DHCP / DNS / TCP 端到端测试可行
# ★ 第 18B 步：增加 VirtIO 块设备，用于可写 OBFS 根文件系统
$QemuArgs = @(
    "-machine", "q35",
    "-m", "1024",
    "-accel", "tcg",
    "-display", "none",
    "-no-reboot",
    "-device", "isa-debug-exit,iobase=0x501,iosize=0x04",
    "-netdev", "user,id=n0",
    "-device", "virtio-net-pci,netdev=n0",
    "-drive", "file=$ObfsImg,if=none,id=hd0,format=raw",
    "-device", "virtio-blk-pci,drive=hd0",
    "-drive", "file=fat:rw:$EspDir,format=raw"
)

if ($OvmfCode) {
    $QemuArgs += @("-drive", "if=pflash,format=raw,readonly=on,file=$OvmfCode")
    $QemuArgs += @("-drive", "if=pflash,format=raw,file=$BuildVars")
} else {
    $QemuArgs += @("-bios", $Ovmf)
}

Write-Host "启动 QEMU..."
Write-Host "  ESP: $EspDir"
Write-Host "  OVMF: $Ovmf"
Write-Host "  OBFS 镜像: $ObfsImg"
Write-Host "  串口日志: $SerialLog"
Write-Host "  网络: user-mode NAT (10.0.2.0/24), eth0=virtio-net-pci"
Write-Host "  存储: vda=virtio-blk-pci (64 MiB)"

& $Qemu @QemuArgs -serial stdio -monitor none 2>&1 | Tee-Object -FilePath $SerialLog

Write-Host "QEMU 已退出，串口日志：$SerialLog"
#===OmniBridgeOs/scripts/run-qemu.ps1 结束===