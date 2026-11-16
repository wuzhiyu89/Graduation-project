param(
    [string]$Port = 'COM21',
    [int]$Baud = 115200,
    [string]$Out = 'C:\Users\wzy886\AppData\Local\Temp\com21_cap.txt',
    [int]$Seconds = 45,
    [string]$Send = '',
    [string]$SendFile = '',
    [int]$SendAfterMs = 600
)

$ErrorActionPreference = 'Stop'

$sp = New-Object System.IO.Ports.SerialPort $Port, $Baud, 'None', 8, 'One'
$sp.ReadTimeout = 500
# Firmware logs Chinese as UTF-8; SerialPort defaults to ASCII and turns every
# non-ASCII byte into '?'. Must set the encoding explicitly before reading.
$sp.Encoding = New-Object System.Text.UTF8Encoding($false)
$sp.Open()

$fs = [System.IO.File]::Create($Out)
$sw = New-Object System.IO.StreamWriter($fs, (New-Object System.Text.UTF8Encoding($false)))

# 可选的发送：打开端口后等一小会儿再发，避免和启动日志抢
# ★-SendFile 用于带引号的 JSON（#json ...）★ 从 shell 传 '{"cmd":"..."}' 时，
#   bash -> powershell -> SerialPort 每一层都会吃掉或重排引号，实测发到板上变成
#   {cmd:rule.query} 这种非法 JSON。写成文件再用 -SendFile 读，字节完全可控。
$cmd = $Send
$cmdBytes = $null
if ($SendFile -ne '')
{
    # ★必须按【原始字节】写★ 走 -Send/-Encoding 这条路，中文会在
    #   PowerShell -> SerialPort 之间被吃掉（实测 "路面积水{level}CM" 变成
    #   "{level}CM"，中文整段消失而不是变成 '?'，固件收到的是残缺 JSON）。
    $fullPath = [System.IO.Path]::GetFullPath($SendFile)
    Write-Host ("send-file = " + $fullPath)
    $raw = [System.IO.File]::ReadAllBytes($fullPath)
    $len = $raw.Length
    while (($len -gt 0) -and (($raw[$len - 1] -eq 10) -or ($raw[$len - 1] -eq 13))) { $len-- }
    $cmdBytes = [byte[]]::new($len + 2)
    [System.Array]::Copy($raw, $cmdBytes, $len)
    $cmdBytes[$len]     = 13
    $cmdBytes[$len + 1] = 10
    $cmd = ''
}
if (($cmdBytes -ne $null) -or ($cmd -ne ''))
{
    Start-Sleep -Milliseconds $SendAfterMs
    if ($cmdBytes -ne $null) { $sp.Write($cmdBytes, 0, $cmdBytes.Length) }
    else                     { $sp.Write($cmd + "`r`n") }
}

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

$sw.Close()
$fs.Close()
$sp.Close()
Write-Host ('captured -> ' + $Out)
