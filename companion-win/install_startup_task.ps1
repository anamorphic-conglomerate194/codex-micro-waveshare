# Register a Windows scheduled task to run the Codex Watch Companion at logon.
# Run this in YOUR OWN PowerShell (not the sandbox). Requires the user to be
# logged in with codex (auth.json present) and bleak installed (setup script).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File install_startup_task.ps1
#   powershell -ExecutionPolicy Bypass -File install_startup_task.ps1 -Interval 60
param(
    [int]$Interval = 60
)

$ErrorActionPreference = "Stop"
$Dir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Companion = Join-Path $Dir "src\codex_watch_all.py"

# Resolve Python launcher
$py = $null
foreach ($cand in @("py", "python")) {
    try {
        $v = & $cand -c "import sys; print(sys.executable)" 2>$null
        if ($LASTEXITCODE -eq 0 -and $v) { $py = $cand; break }
    } catch { }
}
if (-not $py) { Write-Host "Python not found." -ForegroundColor Red; exit 1 }

$taskName = "CodexWatchCompanion"

# Build the action: hidden window, run companion in --watch mode.
$pyExe = (& $py -c "import sys; print(sys.executable)")
$action = New-ScheduledTaskAction -Execute $pyExe -Argument "`"$Companion`" --watch --interval $Interval" -WorkingDirectory $Dir

# Trigger at logon of the current user.
$trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME

# Run hidden, no admin required.
$settings = New-ScheduledTaskSettingsSet -Hidden -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Hours 0)

# Register (recreate if exists)
$principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Limited
Register-ScheduledTask -TaskName $taskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal -Force | Out-Null

Write-Host "Scheduled task '$taskName' registered." -ForegroundColor Green
Write-Host "It will run at next logon: $pyExe `"$Companion`" --watch --interval $Interval"

# Start it right now so the watch gets quota immediately.
Start-ScheduledTask -TaskName $taskName
Write-Host "Task started now." -ForegroundColor Green

# Show status
Get-ScheduledTask -TaskName $taskName | Select-Object TaskName, State | Format-Table -AutoSize
