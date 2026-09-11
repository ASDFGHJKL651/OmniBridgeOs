$ErrorActionPreference = "Stop"

$MsysRoot = "C:\msys64"
$Bash = Join-Path $MsysRoot "usr\bin\bash.exe"

if (-not (Test-Path $Bash)) {
    Write-Host "未找到 MSYS2：$Bash"
    Write-Host "请从 https://www.msys2.org/ 安装 MSYS2，然后重新运行。"
    exit 1
}

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$MsysProjectRoot = $ProjectRoot -replace '\\','/'

Write-Host "调用 MSYS2 安装/检查工具链..."
& $Bash -lc "cd '$MsysProjectRoot' && ./scripts/setup-toolchain.sh"

if ($LASTEXITCODE -ne 0) {
    throw "工具链安装/检查失败，退出码 $LASTEXITCODE"
}