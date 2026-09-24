$ErrorActionPreference = "Stop"

$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$Log = Join-Path (Join-Path $ProjectRoot "build") "serial.log"

if (-not (Test-Path $Log)) {
    Write-Error "找不到串口日志：$Log"
}

$required = @(
    "Knot booting",
    "OmniBridge OS UEFI boot stub v0.3",
    "Kernel codename: Knot, userspace codename: Tide",
    "[UEFI] kernel.elf loaded",
    "[UEFI] ELF entry=",
    "[UEFI] page tables built",
    "[UEFI] Boot Services exited",
    "[UEFI] handoff to kernel at",
    "[KRN] Hello Kernel",
    "[GDT] loaded",
    "[IDT] loaded",
    "[PMM] usable pages:",
    "[VMM] 4-level paging enabled",
    "Hello Kernel",
    "kmalloc OK"
)

$text = Get-Content -Raw $Log
$missing = @()
foreach ($line in $required) {
    if ($text -notmatch [regex]::Escape($line)) {
        $missing += $line
    }
}

if ($missing.Count -gt 0) {
    Write-Host "串口日志缺少以下内容：" -ForegroundColor Red
    $missing | ForEach-Object { Write-Host "  - $_" }
    exit 1
}

Write-Host "串口日志校验通过。" -ForegroundColor Green
exit 0