$ErrorActionPreference = "Stop"

$MsysBash = "C:\msys64\usr\bin\bash.exe"
if (-not (Test-Path $MsysBash)) {
    throw "未找到 MSYS2 bash：$MsysBash"
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$MsysProjectRoot = $ProjectRoot -replace '\\','/'

Write-Host "在 MSYS2 中执行 make all..."
& $MsysBash -lc "cd '$MsysProjectRoot' && make all"

if ($LASTEXITCODE -ne 0) {
    throw "构建失败，退出码 $LASTEXITCODE"
}

Write-Host "构建完成：build/esp/EFI/BOOT/BOOTX64.EFI"