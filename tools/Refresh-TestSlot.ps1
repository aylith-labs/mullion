# Register the Test slot from the newest build Fetch-CIBuild.ps1 has staged.
#
# The Test-slot counterpart to Promote-DevSlot.ps1, and much simpler: mult is the
# one slot CLAUDE.md says may be registered, launched, restarted or replaced with
# no confirmation needed, so there is no idle check, no waiting for a window to
# close on its own, no "ask first". If mult is open, this closes it -- Windows will
# not re-register a package identity from a different folder while it is running,
# and there is nothing here to ask permission for.
#
# Deliberately not run by the CI poller: swapping mult's binaries out from under an
# actively open test window on a timer would be its own kind of surprise. Run this
# by hand, right before verification is actually needed.
[CmdletBinding()]
Param(
    [string]$SlotRoot = 'C:\TerminalSlots',
    [switch]$NoLaunch
)

$ErrorActionPreference = 'Stop'

$PackageName  = 'WindowsTerminalTest'
$StagedDir    = Join-Path $SlotRoot 'test-staged-ci'
$TestStage    = Join-Path $SlotRoot 'test'
$MarkerPath   = Join-Path $SlotRoot 'test-pending-ci.json'

function Say {
    Param([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::Gray)
    Write-Host $Message -ForegroundColor $Color
}

if (-not (Test-Path (Join-Path $StagedDir 'AppxManifest.xml'))) {
    throw "nothing staged at $StagedDir -- run Fetch-CIBuild.ps1 first"
}

$markerInfo = $null
if (Test-Path $MarkerPath) {
    try { $markerInfo = Get-Content $MarkerPath -Raw | ConvertFrom-Json } catch { $markerInfo = $null }
}
if ($markerInfo) {
    Say "Registering $($markerInfo.commit)$(if ($markerInfo.dirty) { '+dirty' }) ($($markerInfo.branch)), built $($markerInfo.timestampUtc)" ([ConsoleColor]::Cyan)
}

# mult is disposable -- close it outright rather than waiting or asking. Windows
# will not remove a package identity that is still running.
$running = Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like "$TestStage\*" }
if ($running) {
    Say "Closing $($running.Count) running Test slot process(es)" ([ConsoleColor]::DarkGray)
    $running | Stop-Process -Force -ErrorAction SilentlyContinue
    Start-Sleep -Milliseconds 500
}

# Settings and state are worth keeping across a re-register -- same reasoning and
# same settingsHash handling as Deploy-TerminalSlots.ps1's Test registration.
$localStateDir = "$env:LOCALAPPDATA\Packages\WindowsTerminalTest_8wekyb3d8bbwe\LocalState"
$localStateSettings = Join-Path $localStateDir 'settings.json'
$localStateState = Join-Path $localStateDir 'state.json'
$savedSettings = $null
$savedState = $null
if (Test-Path $localStateSettings) { $savedSettings = Get-Content $localStateSettings -Raw -ErrorAction SilentlyContinue }
if (Test-Path $localStateState) { $savedState = Get-Content $localStateState -Raw -ErrorAction SilentlyContinue }

# Always unregister first: re-registering the same version from a DIFFERENT
# folder is a silent no-op that leaves the old build running, and re-registering
# from the SAME folder after the files changed fails outright with 0x80073CFB.
$existing = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue
if ($existing) {
    Say "Unregistering $($existing.PackageFullName)" ([ConsoleColor]::DarkGray)
    Remove-AppxPackage -Package $existing.PackageFullName -ErrorAction Stop
}

$previous = "$TestStage.previous"
if (Test-Path $previous) { Remove-Item -Recurse -Force $previous -ErrorAction SilentlyContinue }
if (Test-Path $TestStage) { Move-Item -LiteralPath $TestStage -Destination $previous -Force }
Move-Item -LiteralPath $StagedDir -Destination $TestStage -Force

Add-AppxPackage -Path (Join-Path $TestStage 'AppxManifest.xml') -Register -ForceUpdateFromAnyVersion

$now = Get-AppxPackage -Name $PackageName -ErrorAction SilentlyContinue
if (-not $now -or (Resolve-Path $now.InstallLocation).Path -ne (Resolve-Path $TestStage).Path) {
    throw "Test slot did not register from $TestStage (still: $($now.InstallLocation))"
}
if (Test-Path $previous) { Remove-Item -Recurse -Force $previous -ErrorAction SilentlyContinue }
Say "Registered from $TestStage" ([ConsoleColor]::Green)

if ($savedSettings -or $savedState) {
    New-Item -ItemType Directory -Force -Path $localStateDir | Out-Null
    if ($savedSettings) {
        Set-Content -Path $localStateSettings -Value $savedSettings -Encoding UTF8
        Say '  restored settings.json' ([ConsoleColor]::DarkGray)
    }
    if ($savedState) {
        try {
            # -AsHashtable: state.json can carry a property named "", which
            # ConvertFrom-Json rejects outright without it.
            $stateObj = $savedState | ConvertFrom-Json -AsHashtable -ErrorAction Stop
            $stateObj.Remove('settingsHash') | Out-Null
            Set-Content -Path $localStateState -Value ($stateObj | ConvertTo-Json -Depth 20) -Encoding UTF8
            Say '  restored state.json (settingsHash dropped to refresh the jump list)' ([ConsoleColor]::DarkGray)
        }
        catch {
            Set-Content -Path $localStateState -Value $savedState -Encoding UTF8
            Say "  restored state.json verbatim (could not parse: $($_.Exception.Message))" ([ConsoleColor]::DarkYellow)
        }
    }
}

if ($NoLaunch) {
    Say 'Start it yourself with: mult' ([ConsoleColor]::DarkGray)
    return
}

Say 'Starting mult to confirm it reaches a window...' ([ConsoleColor]::DarkGray)
Start-Process 'mult.exe' -ErrorAction Stop
$deadline = (Get-Date).AddSeconds(30)
$winner = $null
while ((Get-Date) -lt $deadline -and -not $winner) {
    Start-Sleep -Milliseconds 250
    $winner = Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$TestStage\*" -and $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
}

if ($winner) {
    Say "Boot confirmed (pid $($winner.Id)). Left running for verification." ([ConsoleColor]::Green)
}
else {
    Say 'Did not reach a window within 30s -- check Get-WinEvent -LogName Application (event ID 1000/1001).' ([ConsoleColor]::Red)
    Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$TestStage\*" } |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    exit 1
}
