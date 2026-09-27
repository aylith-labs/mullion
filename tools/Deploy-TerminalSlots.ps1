# Builds and deploys the two local Terminal slots.
#
#   Test slot (mult.exe) -- disposable. Registered immediately; replace and
#                          restart it as often as you like.
#   Dev slot  (muld.exe) -- where real work happens. NOT touched here. Its
#                          payload is staged so the running Dev instance can
#                          offer to promote it when you have no active
#                          sessions, which is the only moment it is safe to
#                          restart.
#
# Both packages are produced from the SAME compiled binaries: Branding.targets
# maps an unrecognised branding to WT_BRANDING_DEV, so a Test build differs from
# a Dev build only in its manifest. What you verify in mult is byte-for-byte the
# code that gets promoted into muld.
#
# That is a property this repo has to actively maintain, not one it gets for
# free. The two packaging passes below pass WindowsTerminalBranding on the
# command line, which makes it a GLOBAL property, and MSBuild forwards global
# properties to every ProjectReference -- so each pass genuinely recompiles the
# C++ into the shared bin\ directory. Any build file that special-cases the
# branding therefore has to list 'Test' wherever it lists 'Dev', or the two
# passes quietly produce different binaries. It has happened: src\
# common.build.post.props and src\host\proxy\Host.Proxy.vcxproj both omitted
# 'Test' and the staged exes differed by 2,560 bytes. The check below is cheap;
# run it if you touch anything branding-conditional.
#
#   (Get-FileHash C:\TerminalSlots\dev-staged\mullion.exe).Hash -eq
#   (Get-FileHash C:\TerminalSlots\test\mullion.exe).Hash
[CmdletBinding()]
Param(
    # Skip the solution build and just repackage/deploy what is already built.
    [switch]$NoBuild,
    # Stage the Dev payload but do not register the Test slot either.
    [switch]$StageOnly,
    # How many projects MSBuild may compile at once. 0 means one per core.
    #
    # Worth turning down when the machine is short of COMMIT rather than cores.
    # Every parallel cl.exe reserves virtual memory for its precompiled header,
    # and this repo uses a PCH everywhere, so a wide build multiplies that
    # reservation by the core count. Past the commit limit it fails as
    # "C3859: Failed to create virtual memory for PCH ... the paging file is too
    # small", which reads like a disk problem and is really "too many compilers
    # at once". A desktop with a big WSL VM resident is the usual way to get
    # there: cores stay free while commit does not.
    [int]$MaxCpuCount = 0
)

$ErrorActionPreference = 'Stop'

$Root      = Split-Path -Parent $PSScriptRoot
$SlotRoot  = 'C:\TerminalSlots'
$TestStage = Join-Path $SlotRoot 'test'
# The Dev payload is staged BESIDE the live one, never over it. Two reasons, and
# the first one is fatal on its own: the live payload's binaries are loaded by
# the running Dev instance, so Windows refuses to overwrite them and the unpack
# dies partway through -- having already replaced every file that wasn't locked.
# That is the worst of both worlds: production left half-new, and the deploy
# aborted before it registered Test. Second, "production changes only when I
# press promote" is the whole doctrine here, and a deploy that rewrites the Dev
# payload breaks it even when nothing is running. Promote-DevSlot.ps1 swaps this
# directory into place once the Dev windows are closed.
$DevLive   = Join-Path $SlotRoot 'dev'
$DevStage  = Join-Path $SlotRoot 'dev-staged'
# Test is unpacked here and swapped into $TestStage during registration, for the
# same reason the Dev payload is staged beside the live one -- see Expand-Slot.
$TestStageNew = Join-Path $SlotRoot 'test-staged'
$Config    = 'Release'
$Platform  = 'x64'

$makeappx = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\*\x64\makeappx.exe' -ErrorAction SilentlyContinue |
    Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
if (-not $makeappx) { throw 'makeappx.exe not found; is the Windows SDK installed?' }

# Don't depend on the caller having already set up the dev environment: without it
# msbuild.exe simply isn't on PATH and the script dies halfway through packaging.
if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
    Write-Host '== setting up MSBuild dev environment ==' -ForegroundColor DarkGray
    Import-Module "$Root\tools\OpenConsole.psm1" -Force
    Set-MsbuildDevEnvironment
    if (-not (Get-Command msbuild.exe -ErrorAction SilentlyContinue)) {
        throw 'msbuild.exe still not on PATH after Set-MsbuildDevEnvironment'
    }
}

# `/m` with no number is one project per core. $MaxCpuCount replaces it when the
# caller wants fewer -- see the parameter above for why cores are not the
# constraint that runs out first.
#
# CL_MPCount has to be capped alongside it. `/m` bounds how many PROJECTS build
# at once; MultiProcessorCompilation independently forks one cl.exe per core
# INSIDE each project, so `/m:4` on a 32-core machine can still be 128 compilers
# and still exhausts commit ("C3859: Failed to create virtual memory for PCH").
# Measured on this machine: a full rebuild died at both `/m` and `/m:4`, and
# survived at `/m:2 /p:CL_MPCount=2`.
#
# Kept as two scalars rather than one array: an array nested in the @(...) that
# builds each argument list gets flattened to a single "/m:6 /p:CL_MPCount=6"
# token, which msbuild rejects with "MSB1030: Maximum CPU count is not valid".
$ParallelSwitch = if ($MaxCpuCount -gt 0) { "/m:$MaxCpuCount" } else { '/m' }
$ClMpSwitch = if ($MaxCpuCount -gt 0) { "/p:CL_MPCount=$MaxCpuCount" } else { '' }

function Invoke-MsBuild {
    Param([string[]]$Arguments)
    # Drop empties so an unset optional switch (see $ClMpSwitch) does not reach
    # msbuild as a blank argument.
    $effective = @($Arguments | Where-Object { $_ })
    & msbuild.exe @effective
    if ($LASTEXITCODE -ne 0) { throw "msbuild failed ($LASTEXITCODE): $($effective -join ' ')" }
}

if (-not $NoBuild) {
    Write-Host '== building solution ==' -ForegroundColor Cyan
    Invoke-MsBuild @("$Root\OpenConsole.slnx", "/p:Configuration=$Config", "/p:Platform=$Platform", $ParallelSwitch, $ClMpSwitch, '/v:m', '/nologo')
}

# The wapproj is built directly for each branding, so SolutionDir has to be passed
# explicitly -- it is only set automatically when building through the .slnx.
$wapproj = "$Root\src\cascadia\CascadiaPackage\CascadiaPackage.wapproj"

# The packaging project caches its generated AppxManifest in obj\. Building two
# brandings back to back can otherwise reuse the previous branding's manifest
# against the current branding's alias stub, and MakeAppx rejects the mismatch
# ("the file name mult.exe ... doesn't exist in the package"). Clearing the
# packaging intermediates is cheap; the C++ underneath does get revisited, but
# with the branding conditions kept in sync it compiles to the same bytes and
# so mostly comes back from the up-to-date check.
$pkgObj = "$Root\src\cascadia\CascadiaPackage\obj"
if (Test-Path $pkgObj) {
    Write-Host "== clearing packaging intermediates ==" -ForegroundColor DarkGray
    Remove-Item -Recurse -Force $pkgObj -ErrorAction SilentlyContinue
}

Write-Host '== packaging Test slot ==' -ForegroundColor Cyan
Invoke-MsBuild @($wapproj, "/p:Configuration=$Config", "/p:Platform=$Platform",
                 '/p:WindowsTerminalBranding=Test', "/p:SolutionDir=$Root\", $ParallelSwitch, $ClMpSwitch, '/v:m', '/nologo')

if (Test-Path $pkgObj) { Remove-Item -Recurse -Force $pkgObj -ErrorAction SilentlyContinue }

Write-Host '== packaging Dev slot (staged, not installed) ==' -ForegroundColor Cyan
Invoke-MsBuild @($wapproj, "/p:Configuration=$Config", "/p:Platform=$Platform",
                 '/p:WindowsTerminalBranding=Dev', "/p:SolutionDir=$Root\", $ParallelSwitch, $ClMpSwitch, '/v:m', '/nologo')

function Expand-Slot {
    Param([string]$MsixDir, [string]$Destination, [string]$Label, [switch]$Fresh)
    $msix = Get-ChildItem $MsixDir -Filter *.msix -ErrorAction SilentlyContinue |
        Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $msix) { throw "no .msix produced for $Label in $MsixDir" }

    # unpack /o overwrites, it doesn't clean: a file dropped from the package
    # stays behind forever. Only worth doing where the directory is ours to
    # empty -- which is the staged payload, never a registered one.
    if ($Fresh -and (Test-Path $Destination)) {
        Remove-Item -Recurse -Force $Destination
    }

    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    & $makeappx unpack /o /p $msix.FullName /d $Destination | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "makeappx unpack failed for $Label" }
    return $msix
}

$pkgBase  = "$Root\src\cascadia\CascadiaPackage\AppPackages"

# NEITHER slot is unpacked over in place. Overwriting a registered payload fails
# with `0x800704c8 - the requested operation cannot be performed on a file with a
# user-mapped section open`: something in the shell keeps the package's
# resources.pri mapped, and a mapped file can be opened but not truncated. It is
# not tied to the app running -- it was hit on 2026-08-29 with no Test process
# alive at all, and it outlived unpinning the app from the taskbar.
#
# Renaming the folder is unaffected (measured), so both slots unpack to a staging
# directory and the registration block swaps the folder into place. That also
# gets `-Fresh` semantics for Test for free: a file dropped from the package no
# longer lingers in the payload forever.
$testMsix = Expand-Slot -MsixDir "$pkgBase\Test\CascadiaPackage_0.0.1.0_${Platform}_Test" -Destination $TestStageNew -Label 'Test' -Fresh
$devMsix  = Expand-Slot -MsixDir "$pkgBase\CascadiaPackage_0.0.1.0_${Platform}_Test"      -Destination $DevStage     -Label 'Dev'  -Fresh

# Starts the registered Test slot and waits for a window. Test and Dev are the
# SAME binary (see header comment), so a build that cannot start is caught here
# rather than by promoting it into the slot that hosts live sessions.
#
# Found the hard way on 2026-08-27: an incremental build left XAML/IDL artifacts
# inconsistent with 8fef97d22's new interfaces, and every locally built slot
# aborted (0xC0000409) inside AppHost::Initialize before painting a window. The
# deploy noticed nothing -- it reported success and staged the corpse for
# promotion. A build that has never been started is not a build that works.
#
# It MUST launch through the mult alias, not the payload exe. Running
# <payload>\mullion.exe directly gives the process no package identity,
# so activating the TerminalApp.App WinRT class fails with 0x80040154
# REGDB_E_CLASSNOTREG -- for a perfectly healthy build. Probing the exe path
# directly reports every build as broken.
#
# $null means "not tested", not "passed" -- see the caller.
function Test-SlotBoot {
    Param(
        [string]$PayloadDir,
        [int]$TimeoutSeconds = 30
    )

    # Only a process under THIS payload can take our handoff. The packaged
    # window class mixes in the package family name (WindowEmperor.cpp), so a
    # running Dev slot has a different single-instance identity and ignores us.
    # Guarding on "any mullion" would skip the check whenever a Dev
    # window is open -- which is nearly always, and would quietly make this
    # verification never run at all.
    $mine = @(Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
              Where-Object { $_.Path -like "$PayloadDir\*" })
    if ($mine) {
        # "A process exists" is not the same as "a window exists". A windowless
        # process still takes our handoff, so it would make this check opt out --
        # and keep opting out on every later run, since nothing clears it. That is
        # precisely how a zombie survives unnoticed for hours. Clear those first,
        # then decide whether anything real is still up.
        & "$PSScriptRoot\Repair-TerminalSlots.ps1" -Slot Test -TestPayload $PayloadDir -GraceSeconds 20 -Force | Out-Host

        $mine = @(Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
                  Where-Object { $_.Path -like "$PayloadDir\*" })
    }
    if ($mine) {
        Write-Host '   skipping boot check: a Test slot process is already running' -ForegroundColor DarkYellow
        Write-Host "   (mult would hand off to pid $($mine[0].Id) instead of starting)" -ForegroundColor DarkYellow
        return $null
    }

    Write-Host '   starting mult to confirm it reaches a window...' -ForegroundColor DarkGray
    # The alias is a stub that exits immediately; the real process is separate,
    # so find it by payload path rather than by the PID we started.
    Start-Process 'mult.exe' -ErrorAction Stop

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $winner = $null
    while ((Get-Date) -lt $deadline -and -not $winner) {
        Start-Sleep -Milliseconds 250
        $winner = Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -like "$PayloadDir\*" -and $_.MainWindowHandle -ne 0 } |
            Select-Object -First 1
    }

    if ($winner) {
        Stop-Process -Id $winner.Id -Force -ErrorAction SilentlyContinue
        return $true
    }

    # Nothing else was running when we started, so this is the build's own
    # failure: it either aborted or hung before creating a window. Check
    # Get-WinEvent -LogName Application (event ID 1000/1001) for the fault.
    Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -like "$PayloadDir\*" } |
        ForEach-Object { Stop-Process -Id $_.Id -Force -ErrorAction SilentlyContinue }
    return $false
}

# The running Dev instance reads this to decide whether a newer build is waiting.
# It sits beside the payload rather than inside it, so staging needs no changes
# to the packaging itself.
$header = Get-Content "$Root\bin\$Config\inc\TerminalBuildInfo.h" -Raw
function Get-Define {
    Param([string]$Name)
    if ($header -match "#define\s+$Name\s+L?`"([^`"]*)`"") { return $Matches[1] }
    if ($header -match "#define\s+$Name\s+([0-9]+)") { return $Matches[1] }
    return 'unknown'
}

$info = [ordered]@{
    commit       = Get-Define 'TERMINAL_BUILD_COMMIT'
    commitFull   = Get-Define 'TERMINAL_BUILD_COMMIT_FULL'
    branch       = Get-Define 'TERMINAL_BUILD_BRANCH'
    dirty        = [int](Get-Define 'TERMINAL_BUILD_DIRTY')
    timestamp    = [int64](Get-Define 'TERMINAL_BUILD_TIMESTAMP')
    timestampUtc = Get-Define 'TERMINAL_BUILD_TIMESTAMP_STRING'
    payload      = $DevStage
}
$infoPath = Join-Path $SlotRoot 'dev-pending.json'

if (-not $StageOnly) {
    Write-Host '== registering Test slot ==' -ForegroundColor Cyan

    # Always unregister first. Two separate failures make this non-optional:
    #   * re-registering the same version from a DIFFERENT folder is a silent no-op
    #     -- it reports success and leaves the slot running the previous build;
    #   * re-registering the same version from the SAME folder after the files
    #     changed fails outright with 0x80073CFB ("the package is already
    #     installed, and reinstallation was blocked ... in development mode").
    # doc/building.md removes the package first for the same reason.
    $existing = Get-AppxPackage -Name 'WindowsTerminalTest' -ErrorAction SilentlyContinue

    # Removing a package deletes its app data. The Test slot's settings.json and
    # state.json are worth keeping across deploys so customizations and migration state persist.
    $testLocalStateDir = "$env:LOCALAPPDATA\Packages\WindowsTerminalTest_8wekyb3d8bbwe\LocalState"
    $testLocalStateSettings = Join-Path $testLocalStateDir 'settings.json'
    $testLocalStateState = Join-Path $testLocalStateDir 'state.json'
    $savedSettings = $null
    $savedState = $null
    if (Test-Path $testLocalStateSettings) {
        $savedSettings = Get-Content $testLocalStateSettings -Raw -ErrorAction SilentlyContinue
    }
    if (Test-Path $testLocalStateState) {
        $savedState = Get-Content $testLocalStateState -Raw -ErrorAction SilentlyContinue
    }

    if ($existing) {
        Write-Host "   unregistering $($existing.PackageFullName)" -ForegroundColor DarkGray
        Remove-AppxPackage -Package $existing.PackageFullName -ErrorAction Stop
    }

    # Swap the freshly unpacked payload into place now that nothing is registered
    # from it. A rename works even when a file inside the outgoing folder still
    # has a mapped section, which is why the unpack could not simply overwrite --
    # see Expand-Slot. The outgoing copy is kept until the new registration has
    # been asserted, so a failure below has something to go back to.
    $testPrevious = "$TestStage.previous"
    if (Test-Path $testPrevious) { Remove-Item -Recurse -Force $testPrevious -ErrorAction SilentlyContinue }
    if (Test-Path $TestStage) { Move-Item -LiteralPath $TestStage -Destination $testPrevious -Force }
    Move-Item -LiteralPath $TestStageNew -Destination $TestStage -Force

    Add-AppxPackage -Path (Join-Path $TestStage 'AppxManifest.xml') -Register -ForceUpdateFromAnyVersion

    $now = Get-AppxPackage -Name 'WindowsTerminalTest' -ErrorAction SilentlyContinue
    if (-not $now -or (Resolve-Path $now.InstallLocation).Path -ne (Resolve-Path $TestStage).Path) {
        throw "Test slot did not register from $TestStage (still: $($now.InstallLocation))"
    }

    # Only now is the outgoing payload safe to drop.
    if (Test-Path $testPrevious) { Remove-Item -Recurse -Force $testPrevious -ErrorAction SilentlyContinue }

    if ($savedSettings -or $savedState) {
        New-Item -ItemType Directory -Force -Path $testLocalStateDir | Out-Null
        if ($savedSettings) {
            Set-Content -Path $testLocalStateSettings -Value $savedSettings -Encoding UTF8
            Write-Host '   restored Test slot settings.json' -ForegroundColor DarkGray
        }
        if ($savedState) {
            # Restore state.json WITHOUT settingsHash. The jump list is rebuilt
            # only when that hash changes (AppLogic::_ProcessLazySettingsChanges),
            # and the jump list lives shell-side, so it outlives the package --
            # while the rasterized icons it points at live in LocalState, which
            # unregistering just deleted. Carrying the hash across a deploy would
            # leave the shell holding a list of paths to files that no longer
            # exist. Dropping it costs one rebuild on next launch and also cures
            # the staleness that let a jump list survive several builds.
            #
            # -AsHashtable is required: state.json can carry a property whose
            # name is the empty string, which ConvertFrom-Json rejects outright.
            # If it will not parse at all, restore it verbatim rather than lose it.
            try {
                $stateObj = $savedState | ConvertFrom-Json -AsHashtable -ErrorAction Stop
                $stateObj.Remove('settingsHash') | Out-Null
                Set-Content -Path $testLocalStateState -Value ($stateObj | ConvertTo-Json -Depth 20) -Encoding UTF8
                Write-Host '   restored Test slot state.json (settingsHash dropped to refresh the jump list)' -ForegroundColor DarkGray
            }
            catch {
                Set-Content -Path $testLocalStateState -Value $savedState -Encoding UTF8
                Write-Host "   restored Test slot state.json verbatim (could not parse: $($_.Exception.Message))" -ForegroundColor DarkYellow
            }
        }
    }

    Write-Host '== verifying it boots ==' -ForegroundColor Cyan
    $script:bootOk = Test-SlotBoot -PayloadDir $TestStage
    if ($bootOk -eq $false) {
        # Test stays registered on purpose -- 'mult' now reproduces the crash for
        # you. What does NOT happen is dev-pending.json getting written or
        # updated below, so this build is never offered for promotion into muld:
        # whatever was staged from an earlier, working deploy (if anything)
        # stays the promotion candidate instead.
        throw @'
Test slot registered, but it crashed or hung before creating a window -- NOT
staging it for Dev promotion. Reproduce with mult; check Get-WinEvent -LogName
Application (event ID 1000/1001) for the fault.

If the fault is an abort (0xC0000409) inside AppHost::Initialize, suspect stale
incremental artifacts rather than the source: a changed .idl/.xaml can leave
generated interfaces inconsistent across DLLs that msbuild considers up to date.
A full solution build (not an incremental one) is what clears it.
'@
    }
    Write-Host $(if ($bootOk) { '   confirmed: it reached a window' } else { '   not verified -- see above; staging anyway' }) -ForegroundColor $(if ($bootOk) { 'Green' } else { 'DarkYellow' })
}
else {
    Write-Host '== -StageOnly: Test slot not (re)registered, boot not verified ==' -ForegroundColor DarkYellow
    $script:bootOk = $null
}

$info | ConvertTo-Json | Set-Content -Path $infoPath -Encoding UTF8

# The promote button in a running Dev window shells out to this, and Refresh-
# TestSlot.ps1 is its Test-slot counterpart for refreshing mult from a CI fetch.
# Both live beside the payloads rather than in the repo so the app -- and a
# machine that only ever fetches CI builds -- has exactly one fixed path to know,
# and so this still works from a checkout that has moved.
Copy-Item "$PSScriptRoot\Promote-DevSlot.ps1" (Join-Path $SlotRoot 'Promote-DevSlot.ps1') -Force
Copy-Item "$PSScriptRoot\Refresh-TestSlot.ps1" (Join-Path $SlotRoot 'Refresh-TestSlot.ps1') -Force

# Same reasoning, for the CI-poll-interval setting on the Compatibility page: the
# Dev slot reconciles the Scheduled Task against it on startup by shelling out to
# a fixed path, not into the repo. Install-CIBuildPoller.ps1 in turn expects
# Fetch-CIBuild.ps1 beside itself, so both travel together.
Copy-Item "$PSScriptRoot\Install-CIBuildPoller.ps1" (Join-Path $SlotRoot 'Install-CIBuildPoller.ps1') -Force
Copy-Item "$PSScriptRoot\Fetch-CIBuild.ps1" (Join-Path $SlotRoot 'Fetch-CIBuild.ps1') -Force

# Every hidden launch above (and the promote/reconcile helpers built into the
# app itself) routes through this to avoid Windows Terminal's default-terminal
# console-delegation flashing a window despite asking to stay hidden.
Copy-Item "$PSScriptRoot\Invoke-Hidden.vbs" (Join-Path $SlotRoot 'Invoke-Hidden.vbs') -Force

Write-Host ''
Write-Host "Test slot  : registered from $TestStage (run: mult)" -ForegroundColor Green
Write-Host "Dev slot   : staged at $DevStage -- NOT installed" -ForegroundColor Yellow
Write-Host "             still running from $DevLive until you promote" -ForegroundColor Yellow
Write-Host "Pending    : $infoPath$(if (-not $bootOk) { ' (boot unverified)' })" -ForegroundColor Yellow
Write-Host "Build      : $($info.commit) on $($info.branch), built $($info.timestampUtc)"
Write-Host ''
Write-Host 'The Dev slot is intentionally left alone. Promote it from the About' -ForegroundColor DarkGray
Write-Host 'dialog in muld when you have no active sessions.' -ForegroundColor DarkGray
