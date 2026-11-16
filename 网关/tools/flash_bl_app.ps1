# Flash bootloader + application onto a fresh board (flash map mode 2).
#
#   BL  -> 0x08000000 (16 KB)
#   App -> 0x08004000 (224 KB), VECT_TAB_OFFSET = 0x4000
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\flash_bl_app.ps1
#   powershell -ExecutionPolicy Bypass -File tools\flash_bl_app.ps1 -NoErase
#   powershell -ExecutionPolicy Bypass -File tools\flash_bl_app.ps1 -RunAfter
#
# -NoErase : keep the existing config/param/device pages (only rewrite BL+App).
#            ★2026-11-12 复核: 这条警告只针对【分区刚改完的第一次烧录】★
#              META/PARAM/CFG 已从 0x0806C000/0x0806E000/0x08070000 平移到
#              0x08074000/0x08076000/0x08078000; 旧地址落在 App-B 代码区。
#              若板上还是旧布局, 必须用【不带 -NoErase 的全擦】+ provision 重建配置。
#              一旦全擦烧录 + provision 做完(本板已完成), 之后只是 App 代码迭代时
#              -NoErase 反而是【正确】选择: BL 16 KB + App 224 KB 的写入范围最远只到
#              0x0803BFFF, 碰不到 0x08074000 之后的 META/PARAM/CFG, 现场凭据不丢。
#            ⚠ 换板 / 不确定板上分区版本时, 一律走全擦 + provision_flash.ps1。
# -RunAfter: issue 'g' at the end (let the app run); default leaves the core
#            halted after reset, so the serial log can be captured first.
#
# NOTE: comments here are intentionally ASCII only. Windows PowerShell 5.1 reads
#       a BOM-less .ps1 as ANSI and non-ASCII bytes can swallow the next line.

param(
    [string]$Device = 'N32H482RE',
    [int]$Speed = 4000,
    [switch]$NoErase,
    [switch]$RunAfter
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$jlink = 'C:\Program Files\SEGGER\JLink_V974\JLink.exe'
if (-not (Test-Path $jlink)) { throw "J-Link not found: $jlink" }

$bl  = Join-Path $root 'MDK-ARM\bin\BL.bin'
$app = Join-Path $root 'MDK-ARM\bin\CDC.bin'
foreach ($f in @($bl, $app))
{
    if (-not (Test-Path $f)) { throw "missing image: $f" }
}
# Size guards mirror FM_BOOT_SIZE / FM_SLOT_SIZE in app/common/flash_map.h (mode 2).
# A stale CDC.bin linked for the old 208 KB slot would otherwise flash silently.
if ((Get-Item $bl).Length -gt (16 * 1024))
{
    throw "BL.bin exceeds 16 KB (FM_BOOT_SIZE)"
}
if ((Get-Item $app).Length -gt (224 * 1024))
{
    throw "CDC.bin exceeds 224 KB (FM_SLOT_SIZE)"
}

Write-Host ('BL  : {0} ({1} bytes)' -f $bl,  (Get-Item $bl).Length)
Write-Host ('App : {0} ({1} bytes)' -f $app, (Get-Item $app).Length)

$lines = @('si SWD', ('speed ' + $Speed), 'connect')
if (-not $NoErase) { $lines += 'r'; $lines += 'erase' }
$lines += ('loadbin "' + $bl + '", 0x08000000')
$lines += ('loadbin "' + $app + '", 0x08004000')
$lines += 'r'
if ($RunAfter) { $lines += 'g' }
$lines += 'qc'

$script = Join-Path $env:TEMP 'flash_bl_app.jlink'
Set-Content -Path $script -Value $lines -Encoding ascii

Write-Host '--- J-Link script ---'
$lines | ForEach-Object { Write-Host ("  " + $_) }
Write-Host '---------------------'

$out = & $jlink -device $Device -if SWD -speed $Speed -autoconnect 1 -CommanderScript $script 2>&1
$text = $out -join "`n"
# Only the interesting lines: keeps the console readable.
$out | Select-String -Pattern 'O\.K\.|Error|FAILED|RDP|Downloading|Comparing|Could not' | ForEach-Object { Write-Host $_ }

# NOTE: 'RDP2 protection is active' / 'Failed to unsecure the device!' is NORMAL on a
#       read-protected board -- the two loadbin calls still succeed. Only a real
#       connect/download failure, or fewer than 2 x O.K., means the flash failed.
if ($text -match 'Error occurred|Could not connect|Cannot connect|no device found')
{
    Write-Host '!! FLASH FAILED (probe/connect)' -ForegroundColor Red
    exit 1
}

$okCount = ([regex]::Matches($text, 'O\.K\.')).Count
if ($okCount -lt 2)
{
    Write-Host ('!! FLASH FAILED -- got {0} x O.K., expected 2 (BL + App)' -f $okCount) -ForegroundColor Red
    exit 1
}

Write-Host ('flash OK ({0} x O.K.)' -f $okCount) -ForegroundColor Green
exit 0
