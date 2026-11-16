# Generate config-bank images from tools\provision_devices.json and flash them.
# Only the two config sectors (0x08078000..0x0807BFFF) are erased; BL/app are untouched.
# 2026-11-12: CFG-A/B moved to 0x08078000 / 0x0807A000 (was 0x08070000 / 0x08072000).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\provision_flash.ps1
#   powershell -ExecutionPolicy Bypass -File tools\provision_flash.ps1 -Json tools\my4.json
#
# The JSON may contain only the domains you want (sensors/devices/meta); the rest
# comes from factory defaults. Requires gcc (host tests already use it).

param(
    [string]$Json   = "tools\provision_devices.json",
    [string]$Prefix = "MDK-ARM\bin\cfg_prov",
    [switch]$NoFlash
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$gen = Join-Path $root '_host_test\provision_gen.exe'
if (-not (Test-Path $gen)) {
    $incs = @(
        '-I_host_test/stub','-Iapp/links/g4','-Iapp/links/lora','-Iapp/links/serial',
        '-Iapp/cloud','-Iapp/model','-Iapp/service','-Iapp/common','-Iapp/core',
        '-Iapp/ota','-Iapp/common/ring_buf','-Iapp/cjson','-Iapp/drivers/lora',
        '-I_host_test','-Iinc'
    )
    $src = @(
        'app/model/config_store.c','app/model/config_parse.c','app/model/config_serialize.c',
        'app/cloud/cloud_json.c','app/common/utils.c','app/cjson/cJSON.c',
        'app/cjson/cjson_port.c','app/common/ring_buf/ring_buf.c',
        '_host_test/host_stub.c','_host_test/provision_gen.c'
    )
    Write-Host 'building provision_gen.exe ...'
    gcc @('-std=c99','-Wall','-Wextra','-Wno-unused-parameter','-Wno-type-limits') `
        @incs @src -o $gen
    if ($LASTEXITCODE -ne 0) { throw 'provision_gen build failed' }
}

& $gen $Json $Prefix
if ($LASTEXITCODE -ne 0) { throw 'provision image generation failed' }

$A = (Resolve-Path "$Prefix`_A.bin").Path
$B = (Resolve-Path "$Prefix`_B.bin").Path
Write-Host "A: $A"
Write-Host "B: $B"
# Each bank is FM_CFG_SIZE = 8 KB (app/common/flash_map.h, mode 2).
foreach ($f in @($A, $B))
{
    if ((Get-Item $f).Length -gt (8 * 1024))
    {
        throw "config bank image exceeds 8 KB (FM_CFG_SIZE): $f"
    }
}

if ($NoFlash) { Write-Host "NoFlash: images generated, J-Link skipped"; exit 0 }

$jlink = 'C:\Program Files\SEGGER\JLink_V974\JLink.exe'
$script = Join-Path $env:TEMP 'provision_flash.jlink'
@(
    'r',
    'erase 0x08078000 0x0807BFFF',
    ('loadbin "{0}", 0x08078000' -f $A),
    ('loadbin "{0}", 0x0807A000' -f $B),
    'r',
    'g',
    'qc'
) | Set-Content -Path $script -Encoding ascii

& $jlink -device N32H482RE -if SWD -speed 4000 -autoconnect 1 -CommanderScript $script
if ($LASTEXITCODE -ne 0) { throw 'J-Link failed' }
Write-Host 'provisioned: config banks written, board rebooted'
