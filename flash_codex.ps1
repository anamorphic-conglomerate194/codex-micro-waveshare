# =============================================================================
# flash_codex.ps1 — 一键烧录 Codex Micro StopWatch (Waveshare 1.75C) 固件
#
# 用法:
#   powershell -ExecutionPolicy Bypass -File flash_codex.ps1            # 自动选串口
#   powershell -ExecutionPolicy Bypass -File flash_codex.ps1 -Port COM5 # 指定串口
#   powershell -ExecutionPolicy Bypass -File flash_codex.ps1 -Bin <路径> # 指定固件
#
# 说明:
#   - 固件默认取 .pio\build\m5stack-stopwatch\firmware.bin
#   - bootloader/partitions 从同一构建目录自动加载
#   - esptool 查找顺序:PATH -> 标准 PlatformIO 安装 -> -PioHome 参数指定
# =============================================================================

param(
    [string]$Port,
    [string]$Bin = "",
    [string]$PioHome = ""   # 自定义 PlatformIO 目录(内含 penv\Scripts\python.exe)
)

$ErrorActionPreference = "Stop"
$script:Root = Split-Path -Parent $MyInvocation.MyCommand.Path

# --- 1. 定位固件文件 --------------------------------------------------------
if (-not $Bin) {
    $Bin = Join-Path $script:Root ".pio\build\m5stack-stopwatch\firmware.bin"
    if (-not (Test-Path $Bin)) {
        # Release 场景:脚本和固件在同一个下载文件夹
        $releaseBin = Get-ChildItem $script:Root -Filter "codex-micro-waveshare*.bin" -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
        if ($releaseBin) { $Bin = $releaseBin.FullName }
    }
}
if (-not (Test-Path $Bin)) {
    Write-Host "固件不存在: $Bin" -ForegroundColor Red
    Write-Host "用法: 指定 -Bin <固件路径>,或把 flash_codex.ps1 和固件放同一文件夹。" -ForegroundColor Yellow
    Write-Host "示例: powershell -ExecutionPolicy Bypass -File flash_codex.ps1 -Bin .\codex-micro-waveshare-v1.0.0.bin" -ForegroundColor Yellow
    exit 1
}
$BuildDir = Split-Path -Parent $Bin
$Bootloader = Join-Path $BuildDir "bootloader.bin"
$Partitions = Join-Path $BuildDir "partitions.bin"
if (-not (Test-Path $Bootloader) -or -not (Test-Path $Partitions)) {
    Write-Host "缺少 bootloader.bin / partitions.bin(应和 firmware.bin 同目录)" -ForegroundColor Red
    exit 1
}

# --- 2. 定位 esptool --------------------------------------------------------
$pyExe = $null
$esptoolScript = $null

# 2a) 显式指定 PlatformIO home
if ($PioHome) {
    $cand = Join-Path $PioHome "penv\Scripts\python.exe"
    if (Test-Path $cand) { $pyExe = $cand }
}

# 2b) PATH 上的 esptool.py
if (-not $pyExe) {
    $cand = Get-Command esptool.py -ErrorAction SilentlyContinue
    if ($cand) { $esptoolScript = $cand.Source; $pyExe = "python" }
}

# 2c) 标准 PlatformIO 安装
if (-not $pyExe) {
    $cand = Join-Path $env:USERPROFILE ".platformio\penv\Scripts\python.exe"
    if (Test-Path $cand) { $pyExe = $cand }
}

if (-not $pyExe) {
    Write-Host "未找到 esptool。请安装 PlatformIO 或 esptool(python -m pip install esptool)。" -ForegroundColor Red
    Write-Host "提示:可加 -PioHome 'C:\Users\<你>\\.platformio' 指定 PlatformIO 目录。" -ForegroundColor Yellow
    exit 1
}

if (-not $esptoolScript) {
    # PlatformIO 的 esptool.py 位于 packages\tool-esptoolpy\esptool.py
    $esptoolScript = Join-Path (Split-Path (Split-Path $pyExe -Parent) -Parent) "packages\tool-esptoolpy\esptool.py"
    if (-not (Test-Path $esptoolScript)) {
        # 某些安装里 esptool 是 python 模块,直接 -m esptool
        $esptoolScript = ""
    }
}

# --- 3. 选择串口 ------------------------------------------------------------
if (-not $Port) {
    $ports = @([System.IO.Ports.SerialPort]::GetPortNames())
    if ($ports.Count -eq 0) {
        Write-Host "未检测到串口。请用 USB 连接手表(按住 BOOT 再插线可进下载模式)。" -ForegroundColor Red
        exit 1
    }
    if ($ports.Count -gt 1) {
        Write-Host "检测到多个串口: $($ports -join ', ')" -ForegroundColor Yellow
        Write-Host "请用 -Port 参数指定,例如: -Port $($ports[0])" -ForegroundColor Yellow
    }
    $Port = $ports[0]
}
Write-Host "== 烧录固件 ==" -ForegroundColor Cyan
Write-Host "  串口   : $Port"
Write-Host "  固件   : $Bin"
Write-Host "  大小   : $((Get-Item $Bin).Length) bytes"

# --- 4. 执行烧录 ------------------------------------------------------------
# ESP32-S3,16MB flash:bootloader@0x0, partitions@0x8000, app@0x10000
$cmd = @(
    "--chip", "esp32s3",
    "--port", $Port,
    "--baud", "1500000",
    "write_flash", "-z", "--flash_mode", "qio", "--flash_freq", "80m", "--flash_size", "16MB",
    "0x0", $Bootloader,
    "0x8000", $Partitions,
    "0x10000", $Bin
)

if ($esptoolScript) {
    & $pyExe $esptoolScript @cmd
} else {
    & $pyExe -m esptool @cmd
}

if ($LASTEXITCODE -eq 0) {
    Write-Host ""
    Write-Host "== 烧录成功 ==" -ForegroundColor Green
    Write-Host "手表将自动重启。若电脑已配对,等待 ChatGPT 桌面端重新连接即可。" -ForegroundColor Green
} else {
    Write-Host "烧录失败(exit=$LASTEXITCODE)。若提示连接失败,按住 BOOT 键不放再插 USB 重试。" -ForegroundColor Red
    exit $LASTEXITCODE
}
