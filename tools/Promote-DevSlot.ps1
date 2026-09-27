# Promotes the staged payload into the Dev slot, once the Dev slot is no longer
# running.
#
# This is the half of the promotion that cannot happen inside the Terminal:
# re-registering a package identity from a different folder means removing the
# old registration first, and Windows will not remove a running package. The
# Terminal therefore spawns this detached, with its own process id, and exits;
# we wait for it to go, then swap.
#
# Deploy-TerminalSlots.ps1 copies this next to the payloads it stages, so the
# app only has to know one fixed path. Run it by hand if you prefer: with no
# -WaitForPid it waits for every process under the slot to exit, up to ten
# minutes, and then gives up. It never closes anything itself.
#
# Everything it does is announced on the console as well as written to the log.
# A promotion spends nearly all of its life waiting for windows that only the
# user can close, and a wait that prints nothing is indistinguishable from a
# hang -- which is exactly how it was first reported. When the Terminal spawns
# this detached there is no console at all, so nothing here may depend on the
# output being seen; the log remains the record.
[CmdletBinding()]
Param(
    # Wait for this process to exit before touching anything.
    [int]$WaitForPid = 0,
    # Start the Dev slot again once the swap is done.
    [switch]$Relaunch,
    [string]$Payload = 'C:\TerminalSlots\dev',
    # Where the build is waiting. It cannot be written into $Payload: the running
    # Dev instance holds its binaries open, so an unpack over the live payload
    # replaces the unlocked files and then fails. Producers stage beside it and
    # promotion moves it into place.
    #
    # More than one thing stages builds -- a local deploy into dev-staged, a
    # fetched CI build into dev-staged-ci -- so the Terminal passes the one it
    # actually offered. This default is for running the script by hand, and it
    # names the local deploy's directory.
    [string]$Staged = 'C:\TerminalSlots\dev-staged',
    # The marker describing $Staged, cleared once the swap succeeds. Passed for
    # the same reason as $Staged: deleting a hardcoded name would clear some
    # other producer's marker and leave the promoted one still advertising
    # itself, which keeps the button lit in the build you just installed.
    [string]$Marker = '',
    # How many superseded payloads to keep beside the slot, newest first, as
    # dev.rollback.1, dev.rollback.2, and so on.
    #
    # This exists because the swap used to keep the outgoing build only until the
    # new registration was asserted and then delete it. That covers a promotion
    # that fails halfway and covers nothing else -- in particular it does not
    # cover the case that actually happens, a promotion that succeeds onto a
    # build which then turns out not to start. On 2026-09-06 that left the Dev
    # slot unable to launch with no previous payload on disk and no way to fetch
    # an older one, and the only route back was waiting forty minutes for CI.
    #
    # A payload is about 200 MB. Two of them is a rounding error against the cost
    # of not being able to open your terminal.
    [ValidateRange(0, 20)]
    [int]$KeepRollbacks = 2
)

$ErrorActionPreference = 'Stop'

$PackageName = 'WindowsTerminalDev'
$FamilyName  = 'WindowsTerminalDev_8wekyb3d8bbwe'
$Manifest    = Join-Path $Payload 'AppxManifest.xml'
$LogPath     = Join-Path (Split-Path -Parent $Payload) 'promote-dev.log'

function Write-Log {
    Param([string]$Message)
    "$([DateTime]::UtcNow.ToString('u')) $Message" | Add-Content -Path $LogPath -Encoding UTF8
}

# Console and log together, so the person watching and the log afterwards tell
# the same story. Write-Host is a no-op when there is no console, which is the
# detached case, so this is safe to call unconditionally.
function Say {
    Param([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::Gray, [switch]$NoLog)
    Write-Host $Message -ForegroundColor $Color
    if (-not $NoLog -and $Message) { Write-Log $Message }
}

Write-Log "promotion requested (waitForPid=$WaitForPid relaunch=$($Relaunch.IsPresent) payload=$Payload)"

# One promotion at a time, across processes.
#
# Every press of the promote button arms a fresh helper, and each one waits on
# the same Terminal pid - so pressing it twice because nothing seemed to happen
# arms two, and they wake together the moment that pid exits. On 2026-09-09
# three of them woke at once and destroyed each other's work: two logged
# "Removing the old registration" in the same second, one died because the log
# file was locked by a sibling, one swapped the payload and then failed to
# register with "Cannot create a file when that file already exists", and the
# last found nothing left to remove ("Package was not found"). The Dev slot was
# left half-swapped, and because the relaunch is the last statement in the try,
# every one of them threw before reaching it - so the window closed, the build
# changed, and nothing came back.
#
# A named mutex rather than a lock file: it is released by the OS if a helper is
# killed, where a stale file would block every future promotion until deleted by
# hand.
$promotionMutex = New-Object System.Threading.Mutex($false, 'Global\AylithTerminalPromoteDevSlot')
$holdsMutex = $false
try { $holdsMutex = $promotionMutex.WaitOne(0) } catch [System.Threading.AbandonedMutexException] {
    # A previous helper was killed mid-promotion. The mutex is ours now, and the
    # swap below is written to be safe to repeat.
    $holdsMutex = $true
    Write-Log 'took over an abandoned promotion mutex'
}

if (-not $holdsMutex) {
    Write-Log 'another promotion is already running; leaving it to that one'
    Say 'Another promotion is already in progress.' ([ConsoleColor]::DarkGray) -NoLog
    exit 0
}

# Purely so the banner can name the build being promoted. A missing or broken
# marker is not a reason to refuse -- the payload on disk is what gets swapped,
# and the marker only describes it.
#
# Never name anything here $staged: PowerShell variable names are
# case-insensitive, so that assignment lands on the $Staged *path* parameter
# above and every later Join-Path against it fails with "Cannot find drive. A
# drive with the name '@{commit=...}' does not exist" -- which is how this
# silently refused to promote anything at all.
$markerInfo = $null
$markerPath = if ($Marker) { $Marker } else { Join-Path (Split-Path -Parent $Payload) 'dev-pending.json' }
if (Test-Path $markerPath) {
    try { $markerInfo = Get-Content $markerPath -Raw | ConvertFrom-Json } catch { $markerInfo = $null }
}

Write-Host ''
Say 'Promoting the staged build into the Dev slot' ([ConsoleColor]::Cyan) -NoLog
Say ("  payload : {0}" -f $Payload) -NoLog
if ($markerInfo) {
    Say ("  build   : {0}{1} ({2}), built {3}" -f `
        $markerInfo.commit, $(if ($markerInfo.dirty) { '+dirty' } else { '' }), $markerInfo.branch, $markerInfo.timestampUtc) -NoLog
}
Say ("  log     : {0}" -f $LogPath) -NoLog
Write-Host ''

try {
    $stagedManifest = Join-Path $Staged 'AppxManifest.xml'
    $haveStaged = Test-Path $stagedManifest
    if (-not $haveStaged -and -not (Test-Path $Manifest)) {
        throw "no payload to promote: neither $Staged nor $Payload has an AppxManifest.xml"
    }
    if ($haveStaged) { Say ("  staged  : {0}" -f $Staged) -NoLog }

    # Wait for the process that asked for this, then for anything else still
    # running under the slot -- other windows may outlive the one that asked.
    if ($WaitForPid -gt 0) {
        try {
            $p = Get-Process -Id $WaitForPid -ErrorAction Stop
            Write-Log "waiting for pid $WaitForPid"
            $p.WaitForExit()
        } catch [Microsoft.PowerShell.Commands.ProcessCommandException] {
            Write-Log "pid $WaitForPid already gone"
        }
    }

    $pkg = Get-AppxPackage -Name $PackageName
    if (-not $pkg) { throw "$PackageName is not registered" }

    # Match on PATH, not process name: every slot and the real install all run
    # an executable called mullion.exe (WindowsTerminal.exe before the rename).
    $installRoot = $pkg.InstallLocation

    # "Apply on next launch" is the app spawning us against its own pid without
    # asking for a relaunch: it is not quitting, and the whole promise of that
    # button is that the swap happens whenever the user closes the build --
    # tonight, tomorrow, whenever. A ten minute deadline turns that promise into
    # a silent no-op for every session longer than ten minutes, which is nearly
    # all of them. So that request waits for as long as it takes.
    #
    # Every other shape keeps the deadline: "apply now" is quitting immediately
    # so ten minutes is generous, and a hand-run promotion has someone watching
    # a console who deserves an answer rather than a process that never returns.
    $deferred = ($WaitForPid -gt 0) -and (-not $Relaunch)
    $timeout  = [TimeSpan]::FromMinutes(10)
    $started  = Get-Date
    $deadline = if ($deferred) { $null } else { $started + $timeout }

    # Re-announced whenever the set changes rather than once at the top, so that
    # closing one window of several is visibly progress instead of silence.
    $announced = $null
    $ticking   = $false

    # A rewritten line is right in a console and wrong everywhere else: with the
    # output redirected the carriage returns do not rewrite anything and the
    # countdown piles up into a wall of near-identical copies. Measured, not
    # assumed -- it is what the first version of this did when captured.
    $rewrites = $false
    try { $rewrites = -not [Console]::IsOutputRedirected } catch { $rewrites = $false }
    $lastHeartbeat = [DateTime]::MinValue
    while ($true) {
        $live = @(Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
                  Where-Object { $_.Path -and $_.Path.StartsWith($installRoot, [StringComparison]::OrdinalIgnoreCase) })
        if ($live.Count -eq 0) { break }

        $now = Get-Date
        if ($deadline -and $now -gt $deadline) {
            if ($ticking) { Write-Host '' }
            Say ("Gave up: {0} process(es) still running after {1:N0} minutes (pids {2})." -f `
                $live.Count, $timeout.TotalMinutes, ($live.Id -join ',')) ([ConsoleColor]::Red)
            Say 'Nothing was changed. Close the Dev windows and run this again.' ([ConsoleColor]::Red) -NoLog
            exit 3
        }

        $key = (($live.Id | Sort-Object) -join ',')
        if ($key -ne $announced) {
            if ($ticking) { Write-Host ''; $ticking = $false }
            $announced = $key
            Say ("Waiting for {0} Dev process(es) to close before the registration can be swapped:" -f $live.Count) ([ConsoleColor]::Yellow)
            foreach ($l in $live) { Say ("    pid {0}  {1}" -f $l.Id, $l.Path) }
            Say  'Windows will not unregister a package that is still running, so this' ([ConsoleColor]::DarkGray) -NoLog
            Say  'cannot close them for you. Close your Dev Terminal windows and it' ([ConsoleColor]::DarkGray) -NoLog
            Say  'continues on its own.' ([ConsoleColor]::DarkGray) -NoLog
            if ($deadline) {
                Say ("Giving up at {0} if they are still open." -f $deadline.ToString('HH:mm:ss')) ([ConsoleColor]::DarkGray) -NoLog
            }
            else {
                Say 'This waits for as long as it takes; there is no deadline.' ([ConsoleColor]::DarkGray) -NoLog
            }
        }

        # One rewritten line rather than a scrolling wall, so a ten minute wait
        # still reads as one thing happening; a sparse heartbeat when redirected.
        $countdown = if ($deadline) { "{1:mm\:ss} until it gives up" } else { 'no deadline' }
        if ($rewrites) {
            Write-Host ("`r    waiting - {0:mm\:ss} elapsed, $countdown   " -f `
                ($now - $started), $(if ($deadline) { $deadline - $now } else { [TimeSpan]::Zero })) -NoNewline
            $ticking = $true
        }
        elseif (($now - $lastHeartbeat).TotalSeconds -ge 30) {
            $lastHeartbeat = $now
            Write-Host ("    still waiting - {0:mm\:ss} elapsed, $countdown" -f `
                ($now - $started), $(if ($deadline) { $deadline - $now } else { [TimeSpan]::Zero }))
        }

        Start-Sleep -Milliseconds 500
    }
    if ($ticking) { Write-Host '' }
    if ($announced) { Say 'All Dev processes have exited; continuing.' ([ConsoleColor]::Green) }

    # Belt and braces. Remove-AppxPackage -PreserveApplicationData keeps
    # LocalState, but this slot's settings are the whole point of it having a
    # separate identity, so back them up before touching the registration.
    $localState = Join-Path $env:LOCALAPPDATA "Packages\$FamilyName\LocalState\settings.json"
    $backup = $null
    if (Test-Path $localState) {
        $backup = Join-Path $env:TEMP "muld-settings-$([DateTime]::UtcNow.ToString('yyyyMMddHHmmss')).json"
        Copy-Item $localState $backup -Force
        Say ("Backed up the Dev slot's settings.json to {0}" -f $backup)
    }

    Say ("Removing the old registration at {0}" -f $installRoot)
    Remove-AppxPackage -Package $pkg.PackageFullName -PreserveApplicationData

    # Only now is the swap safe: nothing is running out of the payload and
    # nothing is registered from it. Two renames on the same volume, so the
    # window where the slot has no payload at all is a few milliseconds, and the
    # outgoing build is kept until the new registration has been asserted -- if
    # anything below fails, the previous payload is still on disk to go back to.
    $previous = "$Payload.previous"
    if ($haveStaged) {
        if (Test-Path $previous) { Remove-Item -Recurse -Force $previous }
        if (Test-Path $Payload) {
            Move-Item -LiteralPath $Payload -Destination $previous -Force
            Write-Log "moved the outgoing payload to $previous"
        }
        Say ("Swapping in the staged build from {0}" -f $Staged)
        Move-Item -LiteralPath $Staged -Destination $Payload -Force
    }

    Say ("Registering the Dev slot from {0}" -f $Manifest)
    Add-AppxPackage -Path $Manifest -Register

    # -Register is a silent no-op when the identity is still registered
    # elsewhere, so never trust it; assert where we actually landed.
    $landed = (Get-AppxPackage -Name $PackageName).InstallLocation
    if ($landed -ne $Payload) { throw "registration landed at '$landed', expected '$Payload'" }

    Write-Log "registration verified at $landed"

    # The swap is committed. Rotate the outgoing payload into the rollback ring
    # rather than deleting it: this is the copy you need when the build that just
    # registered cleanly turns out not to start.
    try {
        if ($haveStaged -and (Test-Path $previous)) {
            if ($KeepRollbacks -gt 0) {
                # Oldest first, so nothing is overwritten before it has moved up.
                $oldest = Join-Path (Split-Path -Parent $Payload) ("{0}.rollback.{1}" -f (Split-Path -Leaf $Payload), $KeepRollbacks)
                if (Test-Path $oldest) { Remove-Item -LiteralPath $oldest -Recurse -Force -ErrorAction Stop }
                # Parenthesize arithmetic inside -f; otherwise 1 + 1 becomes the suffix "11".
                for ($i = $KeepRollbacks - 1; $i -ge 1; $i--) {
                    $from = Join-Path (Split-Path -Parent $Payload) ("{0}.rollback.{1}" -f (Split-Path -Leaf $Payload), $i)
                    $to   = Join-Path (Split-Path -Parent $Payload) ("{0}.rollback.{1}" -f (Split-Path -Leaf $Payload), ($i + 1))
                    if (Test-Path $from) { [System.IO.Directory]::Move($from, $to) }
                }
                $slot1 = Join-Path (Split-Path -Parent $Payload) ("{0}.rollback.1" -f (Split-Path -Leaf $Payload))
                [System.IO.Directory]::Move($previous, $slot1)
                Write-Log "kept the outgoing payload as $slot1"

                # Record what it was, so a rollback can say what it is going back to
                # rather than leaving you to guess from a timestamp.
                $outgoing = Join-Path $slot1 'build-info.json'
                if (-not (Test-Path $outgoing)) {
                    try {
                        @{ promotedAwayAt = (Get-Date).ToUniversalTime().ToString('yyyy-MM-dd HH:mm:ss') + ' UTC' } |
                            ConvertTo-Json | Set-Content -LiteralPath $outgoing -Encoding UTF8
                    }
                    catch {}
                }
            }
            else {
                Remove-Item -Recurse -Force $previous -ErrorAction SilentlyContinue
            }
        }
    }
    catch {
        # Registration already succeeded. Backup housekeeping must not prevent relaunch.
        Say ("Rollback rotation failed; retained available backups: {0}" -f $_.Exception.Message) ([ConsoleColor]::DarkYellow)
        Write-Log $_.InvocationInfo.PositionMessage
    }
    Say ("Promoted: {0} now runs from {1}" -f $PackageName, $Payload) ([ConsoleColor]::Green)

    if ($backup -and -not (Test-Path $localState)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $localState) | Out-Null
        Copy-Item $backup $localState -Force
        Say 'Restored the Dev slot settings.json'
    }

    # Drop settingsHash so the promoted build rebuilds its jump list.
    #
    # The jump list is regenerated only when that hash changes
    # (AppLogic::_ProcessLazySettingsChanges), and promotion preserves app data,
    # so the hash survives into the new build and suppresses the rebuild --
    # leaving the shell showing a jump list generated by the OLD binaries, with
    # icons that build may no longer produce. Seen on 2026-08-30: a promoted
    # build carrying the glyph-icon fix still showed blank icons because of this.
    #
    # -AsHashtable is required: state.json can carry a property whose name is the
    # empty string, which ConvertFrom-Json rejects. Losing the hash costs one
    # rebuild; losing the file would cost window positions, so leave it alone if
    # it will not parse.
    $stateFile = Join-Path (Split-Path -Parent $localState) 'state.json'
    if (Test-Path $stateFile) {
        try {
            $stateObj = Get-Content $stateFile -Raw -ErrorAction Stop | ConvertFrom-Json -AsHashtable -ErrorAction Stop
            if ($stateObj.ContainsKey('settingsHash')) {
                $stateObj.Remove('settingsHash')
                Set-Content -Path $stateFile -Value ($stateObj | ConvertTo-Json -Depth 20) -Encoding UTF8
                Say 'Cleared settingsHash so the new build refreshes its jump list'
            }
        }
        catch {
            Say ("Left state.json alone (could not parse: {0})" -f $_.Exception.Message) ([ConsoleColor]::DarkYellow)
        }
    }

    # The marker described the payload we just promoted; it is no longer
    # pending, and leaving it would keep the button lit in the new build. Only
    # this one: another producer's marker may describe a build that is still
    # genuinely newer, and the Terminal decides that by timestamp.
    if (Test-Path $markerPath) { Remove-Item $markerPath -Force }

    if ($Relaunch) {
        Say 'Relaunching the Dev slot (muld.exe)'
        Start-Process 'muld.exe'
    }
    else {
        Say 'Start it yourself with: muld' ([ConsoleColor]::DarkGray) -NoLog
    }

    Write-Host ''
    Say 'Promotion complete.' ([ConsoleColor]::Green) -NoLog
    Write-Host ''
    exit 0
}
catch {
    Write-Log "FAILED: $($_.Exception.Message)"
    Write-Log $_.InvocationInfo.PositionMessage
    Write-Host ''
    Write-Host "Promotion failed: $($_.Exception.Message)" -ForegroundColor Red
    Write-Host "See $LogPath" -ForegroundColor Red
    Write-Host ''

    # Bring the Terminal back even though this failed.
    #
    # -Relaunch means the user chose "update and restart", so their windows are
    # already gone by the time we get here - we waited for them to close. Exiting
    # quietly then leaves them with no terminal at all and no idea why, which is
    # strictly worse than a failed promotion they can read about. Whatever state
    # the slot is in, a registered package still starts something.
    if ($Relaunch) {
        Write-Log 'relaunching after a failed promotion so the user is not left without a terminal'
        # A failure can leave the pre-rename registration in place, whose
        # alias is still wtd.exe, so fall back to it.
        try { Start-Process 'muld.exe' }
        catch {
            try { Start-Process 'wtd.exe' } catch { Write-Log "relaunch after failure also failed: $($_.Exception.Message)" }
        }
    }

    exit 1
}
finally {
    if ($holdsMutex) {
        $promotionMutex.ReleaseMutex()
        $promotionMutex.Dispose()
    }
}
