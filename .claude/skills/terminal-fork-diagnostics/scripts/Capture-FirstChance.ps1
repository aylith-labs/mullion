# Capture a symbol-resolved first-chance stack from the Test slot while a setting
# is changed live.
#
# Why this shape: the failures worth debugging in this app are caught, or land a
# tick later during render, so a dump after the fact carries nothing useful - a
# stowed exception's inner error lives on heap a minidump does not capture. The
# throw has to be caught as it happens, which means a debugger attached before
# the trigger.
#
# The trigger is a settings.json write, because AppLogic watches the settings
# FOLDER and hot-reloads. That needs no keyboard and no pointer, so this runs
# without taking the machine away from anyone.
#
#   .\Capture-FirstChance.ps1 -Setting tabPosition -From top -To left
#
# Stage matching PDBs first or every frame of ours reads as
# TerminalApp!DllGetActivationFactory+<offset>:
#
#   .\tools\Fetch-CIBuild.ps1 -RunId <id> -WithSymbols
#   .\tools\Refresh-TestSlot.ps1 -NoLaunch
[CmdletBinding()]
Param(
    # A top-level key in the Test slot's settings.json. String values only; pass
    # -To/-From without quotes and they are written as JSON strings.
    [Parameter(Mandatory = $true)][string]$Setting,
    [Parameter(Mandatory = $true)][string]$From,
    [Parameter(Mandatory = $true)][string]$To,
    [int]$SettleSeconds = 20,
    [string]$SlotRoot = 'C:\TerminalSlots',
    # Leave the window running afterwards instead of letting the debugger take it
    # down. Killing cdb while attached kills the debuggee.
    [switch]$KeepWindow
)

$ErrorActionPreference = 'Stop'

$testPath = Join-Path $SlotRoot 'test'
$symbols  = Join-Path $SlotRoot 'symbols'
$settings = Join-Path $env:LOCALAPPDATA 'Packages\WindowsTerminalTest_8wekyb3d8bbwe\LocalState\settings.json'
$log      = Join-Path $env:TEMP 'wt-firstchance.log'
$cmdFile  = Join-Path $env:TEMP 'wt-firstchance-cmds.txt'

# ShellExecute refuses to launch anything out of C:\Program Files\WindowsApps, so
# the path is resolved and run directly rather than through a shell.
$cdb = Get-ChildItem 'C:\Program Files\WindowsApps' -Directory -Filter 'Microsoft.WinDbg*' -ErrorAction SilentlyContinue |
    ForEach-Object { Join-Path $_.FullName 'amd64\cdb.exe' } |
    Where-Object { Test-Path $_ } |
    Select-Object -First 1
if (-not $cdb) {
    $pkg = (Get-AppxPackage -Name 'Microsoft.WinDbg' -ErrorAction SilentlyContinue | Select-Object -First 1).InstallLocation
    if ($pkg) { $cdb = Join-Path $pkg 'amd64\cdb.exe' }
}
if (-not $cdb -or -not (Test-Path $cdb)) { throw 'cdb.exe not found - install the WinDbg package' }
if (-not (Test-Path $settings)) { throw "no Test-slot settings.json at $settings" }

function Set-Setting([string]$value) {
    $json = Get-Content $settings -Raw
    $pattern = '"' + [regex]::Escape($Setting) + '"\s*:\s*"[^"]*"'
    if ($json -notmatch $pattern) { throw "`"$Setting`" is not present in the Test slot's settings.json" }
    $json = [regex]::Replace($json, $pattern, ('"' + $Setting + '": "' + $value + '"'))
    [System.IO.File]::WriteAllText($settings, $json, (New-Object System.Text.UTF8Encoding $false))
}

function Get-Wtt {
    # By path, always: every Terminal built from this repo shares the process
    # name, including the one this session may be running in.
    Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -like "$testPath\*" -and $_.MainWindowHandle -ne 0 } |
        Select-Object -First 1
}

Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -like "$testPath\*" } |
    ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 3

Set-Setting $From
Start-Process 'mult.exe'
$w = $null
for ($i = 0; $i -lt 40 -and -not $w; $i++) { Start-Sleep -Milliseconds 750; $w = Get-Wtt }
if (-not $w) { throw 'mult never reached a window' }
Write-Host "mult pid $($w.Id), starting state $Setting=$From"
Start-Sleep -Seconds 5

# Three traps, and the order they matter in:
#
#   40080201  RoOriginateError. This is the one to read FIRST when an HRESULT
#             comes back across an ABI boundary. A C++ throw on our side only
#             shows OUR frames - the callee has already returned - so it says
#             what we were doing and never who refused. The originate fires
#             INSIDE the component that rejected the call, so its stack names
#             the actual objector.
#   e06d7363  a C++ throw.
#   c000027b  the XAML stowed-exception fail-fast.
#
# .exr -1 carries the HRESULT a minidump would have lost.
$detach = if ($KeepWindow) { '.detach' } else { '' }
# NOT .symfix. It REPLACES the symbol path with Microsoft's server, throwing
# away the local PDBs that were just staged - which is how a capture can look
# perfectly healthy and still render every frame of ours as
# TerminalApp!DllGetActivationFactory+<huge offset>. That is the export table
# being guessed at, not symbols. _NT_SYMBOL_PATH is already set correctly by
# this script; append to it rather than clobbering it.
#
# And reload only OUR module. A bare `.reload /f` force-reloads every module in
# the process from the symbol server - two and a half minutes here, long enough
# that the trigger fires before `g` runs and the traps never arm. The capture
# then looks like "no exception happened", which is the most misleading result
# this script can produce.
#
# `lm vm` reports what actually loaded, because a symbol path that validates and
# a PDB that matches are different things.
@"
.sympath+ $symbols
.reload /f TerminalApp.dll
.echo ==SYMBOLS==
lm vm TerminalApp
.echo ==ATTACHED==
sxe -c ".echo ==ORIGINATE==; .exr -1; kb 24; gn" 40080201
sxe -c ".echo ==THROW==; .exr -1; kb 40; gn" e06d7363
sxe -c ".echo ==FAILFAST==; .exr -1; kb 60; gn" c000027b
g
$detach
"@ | Set-Content -Path $cmdFile -Encoding ASCII

if (Test-Path $log) { Remove-Item $log -Force }
$env:_NT_SYMBOL_PATH = "$symbols;srv*C:\symbols*https://msdl.microsoft.com/download/symbols"
if (-not (Test-Path $symbols)) {
    Write-Warning "no local symbols at $symbols - our frames will be unnamed. Fetch-CIBuild.ps1 -WithSymbols"
}

# -NoNewWindow so cdb shares this console. A hidden console app would otherwise
# be handed a Windows Terminal window by console delegation.
$proc = Start-Process -FilePath $cdb -ArgumentList @('-p', "$($w.Id)", '-logo', $log, '-cf', $cmdFile) -NoNewWindow -PassThru

for ($i = 0; $i -lt 150; $i++) {
    Start-Sleep -Seconds 1
    if ((Test-Path $log) -and (Select-String -Path $log -Pattern '==ATTACHED==' -Quiet)) { break }
}
Write-Host "cdb attached after ${i}s"
Start-Sleep -Seconds 3

Set-Setting $To
Write-Host "wrote $Setting=$To"
Start-Sleep -Seconds $SettleSeconds

$alive = $null -ne (Get-Process -Id $w.Id -ErrorAction SilentlyContinue)
Write-Host "mult alive after the change: $alive"
if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
Set-Setting $From

Write-Host ''
Write-Host '--- what matters (file, line, HRESULT, message) ---'
if (Test-Path $log) {
    Select-String -Path $log -Pattern 'LogHr|ReturnHr|Msg:\[|==FAILFAST==|second chance' |
        ForEach-Object { '  ' + $_.Line.Trim() }
    Write-Host ''
    Write-Host "full log: $log"
}
else {
    Write-Host '  cdb wrote no log - check that it attached'
}
