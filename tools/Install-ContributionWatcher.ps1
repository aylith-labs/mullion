# Register a Scheduled Task that runs Watch-Contributions.ps1 every 15 minutes:
# a new issue or pull request on aylith-labs/mullion from someone else is
# reviewed adversarially, and only then announced with a desktop notification.
#
#   pwsh -File .\tools\Install-ContributionWatcher.ps1
#   pwsh -File .\tools\Install-ContributionWatcher.ps1 -Uninstall
#
# State, the log and each review live in %LOCALAPPDATA%\MullionWatch.
[CmdletBinding()]
Param(
    [int]$IntervalMinutes = 15,
    [switch]$Uninstall
)

$ErrorActionPreference = 'Stop'

$TaskName = 'Mullion contribution watcher'
$Watcher  = Join-Path $PSScriptRoot 'Watch-Contributions.ps1'

if ($Uninstall) {
    if (Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue) {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
        Write-Host "Removed '$TaskName'." -ForegroundColor Green
    }
    else {
        Write-Host "'$TaskName' is not registered." -ForegroundColor DarkGray
    }
    return
}

if (-not (Test-Path $Watcher)) { throw "Watch-Contributions.ps1 not found beside this script" }
foreach ($tool in 'gh', 'claude') {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool not found on PATH; the watcher needs it." }
}

# Windows PowerShell, not pwsh: the notification uses the WinRT toast API, which
# pwsh 7 cannot load. Hidden through Invoke-Hidden.vbs for the reason its header
# gives (console delegation would otherwise flash a Terminal window).
$powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$hiddenLauncher = Join-Path $PSScriptRoot 'Invoke-Hidden.vbs'
if (-not (Test-Path $hiddenLauncher)) { throw "Invoke-Hidden.vbs not found beside this script" }

$action = New-ScheduledTaskAction -Execute 'wscript.exe' `
    -Argument "`"$hiddenLauncher`" `"$powershell`" -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $Watcher"

$trigger = New-ScheduledTaskTrigger -Once -At (Get-Date).AddMinutes(-1) `
    -RepetitionInterval (New-TimeSpan -Minutes $IntervalMinutes)

# A review can take several minutes; IgnoreNew keeps a slow one from stacking up
# behind itself (the script also holds a mutex for the same reason).
$settings = New-ScheduledTaskSettingsSet `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit (New-TimeSpan -Minutes 30)

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Description 'Reviews new issues and pull requests on aylith-labs/mullion, then raises a notification.' `
    -Force | Out-Null

Write-Host "Registered '$TaskName' every $IntervalMinutes minutes." -ForegroundColor Green
Write-Host "  watcher : $Watcher" -ForegroundColor DarkGray
Write-Host "  state   : $(Join-Path $env:LOCALAPPDATA 'MullionWatch')" -ForegroundColor DarkGray
Write-Host "  run now : Start-ScheduledTask -TaskName '$TaskName'" -ForegroundColor DarkGray
