$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$BuildDir = Join-Path $ProjectRoot "build"
$EspDir = Join-Path $BuildDir "esp"
$SerialLog = Join-Path $BuildDir "serial.log"

New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$Qemu = $env:QEMU_BIN
if (-not $Qemu) {
    $cmd = Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue
    if (-not $cmd) {
        $cmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
    }
    if (-not $cmd) {
        throw "未找到 qemu-system-x86_64。请安装 QEMU 或设置 QEMU_BIN。"
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
        if (Test-Path $c) {
            $Ovmf = $c
            break
        }
    }
}

if (-not $Ovmf) {
    throw "未找到 OVMF。请设置 OVMF_PATH，或安装 MSYS2 edk2-ovmf 包。"
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
        if (Test-Path $v) {
            $OvmfVars = $v
            break
        }
    }

    if (-not $OvmfVars) {
        throw "找到 OVMF CODE，但未找到 VARS 文件。"
    }

    $BuildVars = Join-Path $BuildDir "OVMF_VARS.fd"
    Copy-Item $OvmfVars $BuildVars -Force
}

$QemuArgs = @(
    "-machine", "q35",
    "-m", "1024",
    "-accel", "tcg",
    "-display", "none",
    "-no-reboot",
    "-device", "isa-debug-exit,iobase=0x501,iosize=0x04",
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
Write-Host "  串口日志: $SerialLog"

& $Qemu @QemuArgs -serial stdio -monitor none 2>&1 | Tee-Object -FilePath $SerialLog

Write-Host "QEMU 已退出。串口日志：$SerialLog"