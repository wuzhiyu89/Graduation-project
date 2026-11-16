param(
    # 2026-11-12 实测: COM21 = USB-SERIAL CH340；115200 8N1 与固件 log 口一致
    # (app/log/log.c:102、bsp/src/log.c:112 都是 BaudRate = 115200)。
    # 换板子/换 USB 口会变，用 -Port 覆盖即可，不必改脚本。
    [string]$Port = 'COM21',
    [int]$Baud = 115200,
    [int]$Seconds = 6,
    [string]$Out = '',
    [int]$SendAfterMs = 400
)

# 发一条控制台命令并抓回显。
#
# 【为什么单开一个脚本】_capture_com.ps1 的 -SendFile 在 powershell.exe -File
#   模式下传进来会是 $null（-ne '' 又对它判真，坑很深）。这里不给调用方任何
#   机会传错：命令固定从本目录的 _cmd.txt 按【原始字节】读，避免中文在
#   PowerShell -> SerialPort 之间被吃掉（实测 "路面积水{level}CM" 会变成
#   "{level}CM"，中文整段消失而不是变 '?'，固件收到残缺 JSON 还照样写配置）。

$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$cmdPath = Join-Path $here '_cmd.txt'
if ($Out -eq '') { $Out = Join-Path $env:TEMP 'com_cmd.txt' }

$raw = [System.IO.File]::ReadAllBytes($cmdPath)
$len = $raw.Length
while (($len -gt 0) -and (($raw[$len - 1] -eq 10) -or ($raw[$len - 1] -eq 13))) { $len-- }
if ($len -eq 0) { throw 'command file is empty' }

$buf = [byte[]]::new($len + 2)
[System.Array]::Copy($raw, $buf, $len)
$buf[$len]     = 13
$buf[$len + 1] = 10

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 500
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)
$sp.Open()

$fs = [System.IO.File]::Create($Out)
$sw = New-Object System.IO.StreamWriter($fs, (New-Object System.Text.UTF8Encoding($false)))

Start-Sleep -Milliseconds $SendAfterMs
$sp.Write($buf, 0, $buf.Length)
Write-Host ("sent {0} bytes from _cmd.txt via {1} @{2}" -f $len, $Port, $Baud)

$end = (Get-Date).AddSeconds($Seconds)
while ((Get-Date) -lt $end)
{
    try
    {
        $s = $sp.ReadExisting()
        if ($s.Length -gt 0) { $sw.Write($s); $sw.Flush() }
    }
    catch { }
    Start-Sleep -Milliseconds 50
}

$sw.Close(); $fs.Close(); $sp.Close()
Write-Host ('captured -> ' + $Out)
