# Setup + run Codex Watch Companion on the user's own Windows environment.
# Run this in YOUR OWN PowerShell (not the sandbox) so network/auth/sqlite
# all work normally.
#
# What it does:
#   1. Installs bleak (BLE) into a user-local Python environment if missing.
#   2. Runs the companion in --watch mode: reads real Codex quota and writes
#      it to the watch over BLE every N seconds.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File setup_win_companion.ps1
#   powershell -ExecutionPolicy Bypass -File setup_win_companion.ps1 -Interval 60
param(
    [int]$Interval = 60,
    [switch]$Demo,
    [switch]$JsonOnly
)

$ErrorActionPreference = "Stop"
$script:Dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$script:Companion = Join-Path $script:Dir "src\codex_watch_companion.py"

Write-Host "== Codex Watch Companion (Windows) setup ==" -ForegroundColor Cyan

# 1) Find a Python launcher (prefer `py`, fall back to `python`)
$py = $null
foreach ($cand in @("py", "python")) {
    try {
        $v = & $cand -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $v) { $py = $cand; break }
    } catch { }
}
if (-not $py) {
    Write-Host "Python not found. Install Python 3.10+: https://www.python.org/downloads/" -ForegroundColor Red
    exit 1
}
Write-Host "Using Python: $py"

# 2) Ensure bleak is importable (install into user site-packages)
& $py -c "import bleak" 2>$null
if ($LASTEXITCODE -ne 0) {
    Write-Host "Installing bleak (BLE support)..."
    & $py -m pip install --user bleak
    if ($LASTEXITCODE -ne 0) { Write-Host "bleak install failed" -ForegroundColor Red; exit 1 }
}

# 3) Check codex CLI
$codex = Get-Command codex -ErrorAction SilentlyContinue
if (-not $codex) {
    Write-Host "codex CLI not found. Install first: npm install -g @openai/codex" -ForegroundColor Red
    exit 1
}
Write-Host "codex: $($codex.Source)"

# 4) Check login
& codex login status 2>$null | Out-Null
if ($LASTEXITCODE -ne 0) {
    Write-Host "codex not logged in. Run: codex login" -ForegroundColor Yellow
    exit 1
}

# 5) Run companion
$runArgs = New-Object System.Collections.Generic.List[string]
if ($JsonOnly) {
    $runArgs.Add("--json-only")
} elseif ($Demo) {
    $runArgs.Add("--demo")
} else {
    $runArgs.Add("--watch")
    $runArgs.Add("--interval")
    $runArgs.Add("$Interval")
}
$runArgs.Add("--verbose")

Write-Host "Running companion: python $script:Companion $($runArgs -join ' ')" -ForegroundColor Cyan
& $py $script:Companion @runArgs
exit $LASTEXITCODE
