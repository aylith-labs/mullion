# Points the Dev slot at C:\TerminalSlots\dev -- the payload Deploy-TerminalSlots.ps1
# stages -- instead of at whatever AppPackages\loose folder happens to sit inside a
# worktree.
#
# Registering out of a checkout is a trap: the folder moves when a worktree moves, it
# is rewritten by the next build, and switching branches silently swaps the binaries
# under a registered package. The slot root is stable and belongs to no branch.
#
# This CANNOT run while the Dev slot is open. Re-registering the same identity from a
# different folder is not an update -- the old registration has to be removed first,
# and Windows will not remove a running package. The script refuses rather than
# terminating your windows; closing them is your call.
[CmdletBinding()]
Param(
    # Register from somewhere else (e.g. a worktree's loose layout) instead.
    [string]$Payload = 'C:\TerminalSlots\dev',
    # Deploy-TerminalSlots.ps1 stages the new build beside the live payload
    # rather than over it -- the running Dev instance keeps its binaries open,
    # so writing into the live payload leaves it half-replaced. If a staged
    # build is waiting, it is moved into place here, once the slot is closed
    # and unregistered. Point this at nothing to register the payload as-is.
    [string]$Staged = 'C:\TerminalSlots\dev-staged'
)

$ErrorActionPreference = 'Stop'

$PackageName = 'WindowsTerminalDev'
$FamilyName  = 'WindowsTerminalDev_8wekyb3d8bbwe'
$Settings    = Join-Path $env:LOCALAPPDATA "Packages\$FamilyName\LocalState\settings.json"
$Manifest    = Join-Path $Payload 'AppxManifest.xml'

$haveStaged = $Staged -and (Test-Path (Join-Path $Staged 'AppxManifest.xml'))
if (-not $haveStaged -and -not (Test-Path $Manifest)) {
    throw "No AppxManifest.xml under $Staged or $Payload -- has the payload been staged?"
}

# Refuse while it is open. Match on path, not process name: the installed Terminal and
# every other slot share the name mullion.exe (WindowsTerminal.exe before the rename).
$running = @(Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue | Where-Object {
    $p = $null; try { $p = $_.Path } catch {}
    $p -and (Get-AppxPackage $PackageName | ForEach-Object { $p.StartsWith($_.InstallLocation, [StringComparison]::OrdinalIgnoreCase) }) -contains $true
})
if ($running) {
    $running | ForEach-Object { Write-Host "  pid=$($_.Id) $($_.Path)" }
    Write-Host ''
    Write-Host "The Dev slot is open. Close those windows and run this again."
    exit 3
}

# Removing a package deletes its app data, which is where settings.json lives.
# -PreserveApplicationData should keep it; back it up anyway, because losing a
# hand-tuned settings.json to a deployment detail is not a recoverable mistake.
$backup = $null
if (Test-Path $Settings) {
    $backup = Join-Path $env:TEMP "settings.json.$(Get-Date -Format yyyyMMdd-HHmmss).bak"
    Copy-Item $Settings $backup -Force
    Write-Host "settings backed up to $backup"
}

$existing = Get-AppxPackage $PackageName
if ($existing) {
    Write-Host "unregistering $($existing.PackageFullName) (was $($existing.InstallLocation))"
    Remove-AppxPackage -Package $existing.PackageFullName -PreserveApplicationData
}

# Swap only now: nothing runs out of the payload and nothing is registered from
# it, so these are two renames on the same volume. The outgoing build is kept
# under .previous until the registration below has been asserted.
$previous = "$Payload.previous"
if ($haveStaged) {
    if (Test-Path $previous) { Remove-Item -Recurse -Force $previous }
    if (Test-Path $Payload) { Move-Item -LiteralPath $Payload -Destination $previous -Force }
    Write-Host "swapping in the staged build from $Staged"
    Move-Item -LiteralPath $Staged -Destination $Payload -Force
}

Write-Host "registering from $Payload"
Add-AppxPackage -Path $Manifest -Register

# Assert. Add-AppxPackage -Register reports success and changes nothing when the same
# identity and version is already registered elsewhere, so a silent no-op has to be
# turned into a hard failure or you end up running stale binaries indefinitely.
$now = Get-AppxPackage $PackageName
if (-not $now) { throw 'Registration reported success but the package is not registered.' }
if ($now.InstallLocation.TrimEnd('\') -ne $Payload.TrimEnd('\')) {
    throw "Registered from $($now.InstallLocation), expected $Payload."
}

if ($haveStaged -and (Test-Path $previous)) { Remove-Item -Recurse -Force $previous -ErrorAction SilentlyContinue }

if ($backup -and -not (Test-Path $Settings)) {
    New-Item -ItemType Directory -Force -Path (Split-Path $Settings) | Out-Null
    Copy-Item $backup $Settings -Force
    Write-Host 'settings.json was dropped by the uninstall; restored from the backup.'
}

Write-Host ''
Write-Host "Dev slot : $($now.InstallLocation) (run: muld)"
Write-Host "Binaries : $((Get-Item (Join-Path $Payload 'mullion.exe')).LastWriteTime)"
