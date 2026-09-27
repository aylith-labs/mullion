# Stage the newest CI-built Dev and Test payloads.
#
# The counterpart to Deploy-TerminalSlots.ps1 on a machine that should not be
# compiling Terminal. It ends in the same state a local deploy leaves behind --
# an unpacked payload on disk and a marker describing it, for each slot -- but
# in its own directories and under its own marker names, so both a local deploy
# and a CI fetch can have staged something at once and each consumer picks
# whichever is newer. Neither producer has to know about the other.
#
# It never touches a live payload. Registering either slot is a deliberate next
# step, not a side effect of staging: Promote-DevSlot.ps1 (Dev, from inside muld)
# or Refresh-TestSlot.ps1 (Test, on demand) -- this just puts a candidate where
# each of those can find it.
#
# Safe to run on a timer: the common case is "nothing newer", which costs one API
# call and no download. See Install-CIBuildPoller.ps1.
#
# pwsh 7 only: under Windows PowerShell 5.1 it dies with a bare null-method error.
#requires -Version 7.0
[CmdletBinding()]
Param(
    [string]$Repo = 'aylith-labs/mullion',
    [string]$Workflow = 'build.yml',
    [string]$Branch = 'main',
    [string]$SlotRoot = 'C:\TerminalSlots',
    # Re-download even when the newest run is one we have already staged.
    [switch]$Force,
    # Stage a specific run instead of the newest successful one. Either the run
    # id from `Invoke-HiddenGitHubCli run list`, or any prefix of a commit sha.
    #
    # The point is going backwards. Without this the only build reachable is the
    # newest green one, so when a promoted build turns out not to start there is
    # nothing to fetch but the thing that just broke, and the only way back is to
    # wait out another CI cycle. That happened on 2026-09-06 and cost an evening.
    #
    # Naming a run bypasses the "is it newer than what we staged" check, since
    # going back to an older build is the whole intent.
    [string]$RunId = '',
    [string]$Commit = '',
    # Also fetch the PDBs build.yml uploads as 'dev-symbols', into
    # $SlotRoot\symbols, replacing whatever was there.
    #
    # Off by default because they are an order of magnitude larger than the
    # payload and only matter while something is being debugged -- and because the
    # poller runs this on a timer. Pass it when you are about to read a stack:
    # without symbols a crash in mult decodes to TerminalApp+0x<offset> and names
    # nothing of ours, which is what made the tabPosition "left" fail-fast
    # unreadable on 2026-09-08.
    #
    # The directory is flat and replaced wholesale rather than keyed on the
    # commit, because symbols are only ever wanted for the build just staged, and
    # stale PDBs beside a newer payload resolve to plausible wrong functions.
    [switch]$WithSymbols
)

$ErrorActionPreference = 'Stop'

$ArtifactName     = 'dev-payload'
$SymbolArtifact   = 'dev-symbols'
$StageDir         = Join-Path $SlotRoot 'dev-staged-ci'
$MarkerPath       = Join-Path $SlotRoot 'dev-pending-ci.json'
$TestStageDir     = Join-Path $SlotRoot 'test-staged-ci'
$TestMarkerPath   = Join-Path $SlotRoot 'test-pending-ci.json'
$SymbolDir        = Join-Path $SlotRoot 'symbols'

function Say {
    Param([string]$Message, [ConsoleColor]$Color = [ConsoleColor]::Gray)
    Write-Host $Message -ForegroundColor $Color
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue)) {
    throw 'gh is not on PATH. Install the GitHub CLI and run `gh auth login`.'
}

# A hidden PowerShell process has no console for native children to inherit.
# Calling gh directly can allocate one and trigger Terminal delegation.
$GitHubCliPath = (Get-Command gh -CommandType Application -ErrorAction Stop).Source
function Invoke-HiddenGitHubCli {
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $GitHubCliPath
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $args) { $startInfo.ArgumentList.Add([string]$argument) }
    $process = [System.Diagnostics.Process]::Start($startInfo)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $script:LASTEXITCODE = $process.ExitCode
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        if ($output) { Write-Output $output.TrimEnd() }
        if ($errors) { Write-Error $errors.TrimEnd() -ErrorAction Continue }
    }
    finally { $process.Dispose() }
}

# Helpers are part of the update, too. A once-installed copy otherwise stays stale forever.
function Sync-SlotHelpers {
    Param([string]$SourceDirectory)
    foreach ($helperName in 'Promote-DevSlot.ps1', 'Refresh-TestSlot.ps1', 'Invoke-Hidden.vbs', 'Install-CIBuildPoller.ps1', 'Fetch-CIBuild.ps1') {
        $source = Join-Path $SourceDirectory $helperName
        $destination = Join-Path $SlotRoot $helperName
        if (-not (Test-Path -LiteralPath $source)) { continue }
        if ([IO.Path]::GetFullPath($source) -eq [IO.Path]::GetFullPath($destination)) { continue }
        if ((Test-Path -LiteralPath $destination) -and (Get-FileHash -LiteralPath $source).Hash -eq (Get-FileHash -LiteralPath $destination).Hash) { continue }
        New-Item -ItemType Directory -Force -Path $SlotRoot | Out-Null
        $helperTemp = "$destination.$([guid]::NewGuid().ToString('N')).tmp"
        try {
            Copy-Item -LiteralPath $source -Destination $helperTemp -Force
            Move-Item -LiteralPath $helperTemp -Destination $destination -Force
        }
        finally { if (Test-Path -LiteralPath $helperTemp) { Remove-Item -LiteralPath $helperTemp -Force } }
    }
}

# Separate from the payload download so it can run on its own: asking for symbols
# against a build that is already staged is the common case (you only want them
# once a crash has happened), and the early "nothing to do" exit below would
# otherwise skip them.
#
# Never fatal. A run from before build.yml grew the symbols step has no such
# artifact, and that is a reason to say so rather than to fail a fetch whose
# payload half succeeded.
function Get-CISymbols {
    Param([string]$RunDatabaseId, [string]$Sha)

    $temp = Join-Path ([System.IO.Path]::GetTempPath()) ("wt-sym-" + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $temp | Out-Null
    try {
        Invoke-HiddenGitHubCli run download $RunDatabaseId --repo $Repo --name $SymbolArtifact --dir $temp 2>$null
        if ($LASTEXITCODE -ne 0) {
            Say "No '$SymbolArtifact' artifact on run $RunDatabaseId (older build, or it expired)." ([ConsoleColor]::DarkYellow)
            return
        }

        $pdbs = @(Get-ChildItem $temp -Filter '*.pdb' -File -ErrorAction SilentlyContinue)
        if ($pdbs.Count -eq 0) {
            Say "'$SymbolArtifact' held no .pdb files." ([ConsoleColor]::DarkYellow)
            return
        }

        if (Test-Path $SymbolDir) { Remove-Item -Recurse -Force $SymbolDir }
        New-Item -ItemType Directory -Force -Path $SymbolDir | Out-Null
        foreach ($pdb in $pdbs) { Move-Item $pdb.FullName (Join-Path $SymbolDir $pdb.Name) -Force }

        # So a stack read six weeks later cannot silently be read against the
        # wrong build.
        Set-Content -Path (Join-Path $SymbolDir 'commit.txt') -Value $Sha -Encoding UTF8

        $mb = [math]::Round((($pdbs | Measure-Object Length -Sum).Sum / 1MB), 1)
        Say "Symbols: $($pdbs.Count) pdb, $mb MB -> $SymbolDir" ([ConsoleColor]::Green)
        Say "  cdb/windbg: set _NT_SYMBOL_PATH to include $SymbolDir" ([ConsoleColor]::DarkGray)
    }
    finally {
        Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
    }
}

$pinned = $RunId -or $Commit

# --status success on purpose: a run that is still going has no artifact yet, and
# one that failed has nothing worth installing. Asking for the newest SUCCESSFUL
# run means a broken commit never displaces a working staged build.
#
# When a run is named, ask for a window of recent successful runs and pick out of
# it, so -Commit can match a prefix. Still success-only: an artifact is what we
# are after and only successful runs have one.
$errFile = [System.IO.Path]::GetTempFileName()
if ($pinned) {
    $runJson = Invoke-HiddenGitHubCli run list --repo $Repo --workflow $Workflow --branch $Branch `
        --status success --limit 60 --json 'databaseId,headSha,updatedAt' 2>$errFile
}
else {
    $runJson = Invoke-HiddenGitHubCli run list --repo $Repo --workflow $Workflow --branch $Branch `
        --status success --limit 1 --json 'databaseId,headSha,updatedAt' 2>$errFile
}
$code = $LASTEXITCODE
$errText = (Get-Content $errFile -Raw -ErrorAction SilentlyContinue)
Remove-Item $errFile -Force -ErrorAction SilentlyContinue

if ($code -ne 0) {
    # Until the workflow has run once -- or while it exists only on a branch that
    # is not the default one -- gh exits non-zero saying it found no such
    # workflow. On a timer that is the steady state of a machine waiting for its
    # first CI build, not a fault, and throwing every quarter of an hour would
    # make a working setup look broken. Anything else (no auth, no network) still
    # fails loudly, because those do need attention.
    # gh reports a missing workflow and a missing REPO with byte-identical text
    # -- "workflow build.yml not found on the default branch" either way, with
    # only the URL differing -- so the message cannot be trusted to tell the
    # benign case from a typo'd repo or revoked auth. Prove the repo resolves
    # before calling it benign; otherwise a silent poller would report "no builds
    # yet" forever against a repo that does not exist.
    if ($errText -match 'not found on the default branch|could not find any workflows|no runs found') {
        Invoke-HiddenGitHubCli repo view $Repo --json name > $null 2>&1
        if ($LASTEXITCODE -eq 0) {
            Say 'No CI builds yet.'
            return
        }
        throw "cannot reach $Repo -- check the repo name, and gh auth status"
    }
    throw "Invoke-HiddenGitHubCli run list failed ($code): $errText"
}

$runs = @($runJson | ConvertFrom-Json)

if ($pinned) {
    if ($RunId) {
        $run = $runs | Where-Object { "$($_.databaseId)" -eq $RunId } | Select-Object -First 1
        if (-not $run) { throw "run $RunId is not among the recent successful $Workflow runs on $Branch" }
    }
    else {
        $matched = @($runs | Where-Object { $_.headSha -like "$Commit*" })
        if ($matched.Count -eq 0) { throw "no recent successful run for a commit starting '$Commit'" }
        if ($matched.Count -gt 1) {
            throw ("'{0}' matches {1} runs ({2}) -- give more of the sha or use -RunId" -f
                $Commit, $matched.Count, (($matched | ForEach-Object { $_.headSha.Substring(0, 9) }) -join ', '))
        }
        $run = $matched[0]
    }
    Say ("Pinned to run {0} ({1})" -f $run.databaseId, $run.headSha.Substring(0, 9)) ([ConsoleColor]::Yellow)
}
else {
    $run = $runs | Select-Object -First 1
}

if (-not $run) {
    Say 'No successful run to fetch.'
    return
}

# The cheap exit, and the one that runs almost every time. Compare against what
# is already staged rather than what is running: re-downloading a payload we
# already have on disk is the waste worth avoiding, and whether it is worth
# PROMOTING is the Terminal's decision, not ours.
# A pinned run skips this: naming an older build is a deliberate request to go
# back to it, and "you already have something newer" is not a reason to refuse.
if (-not $pinned -and -not $Force -and (Test-Path $MarkerPath)) {
    $existing = $null
    try { $existing = Get-Content $MarkerPath -Raw | ConvertFrom-Json } catch { $existing = $null }
    # Also require test-staged-ci to exist: a marker written before this script
    # learned to keep the Test half would otherwise look "already staged" forever
    # and the Test payload would never get backfilled.
    if ($existing -and $existing.commitFull -eq $run.headSha -and (Test-Path $StageDir) -and (Test-Path $TestStageDir)) {
        Sync-SlotHelpers -SourceDirectory $PSScriptRoot
        Say "Already staged: $($run.headSha.Substring(0,9)). Nothing to do."
        if ($WithSymbols) { Get-CISymbols -RunDatabaseId $run.databaseId -Sha $run.headSha }
        return
    }
}

Say "Fetching run $($run.databaseId) ($($run.headSha.Substring(0,9)))" ([ConsoleColor]::Cyan)

$temp = Join-Path ([System.IO.Path]::GetTempPath()) ("wt-ci-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $temp | Out-Null
try {
    Invoke-HiddenGitHubCli run download $run.databaseId --repo $Repo --name $ArtifactName --dir $temp 2>$null
    if ($LASTEXITCODE -ne 0) { throw "Invoke-HiddenGitHubCli run download failed ($LASTEXITCODE) for run $($run.databaseId)" }

    $payloadSrc = Join-Path $temp 'dev-staged'
    $markerSrc  = Join-Path $temp 'dev-pending.json'
    if (-not (Test-Path (Join-Path $payloadSrc 'AppxManifest.xml'))) {
        throw "artifact has no AppxManifest.xml under dev-staged; not a usable payload"
    }

    # Replace wholesale rather than copying over: a file dropped from the package
    # would otherwise survive forever in the staged copy. Same reason
    # Deploy-TerminalSlots.ps1 unpacks its staged payload -Fresh.
    if (Test-Path $StageDir) { Remove-Item -Recurse -Force $StageDir }
    New-Item -ItemType Directory -Force -Path $SlotRoot | Out-Null
    Move-Item $payloadSrc $StageDir

    # The artifact's marker already carries the commit, branch and build time the
    # binaries were stamped with, so nothing is reconstructed here. Only the two
    # fields that are about THIS machine change: where the payload landed, and
    # which producer put it there.
    $info = Get-Content $markerSrc -Raw | ConvertFrom-Json
    $info | Add-Member -NotePropertyName payload -NotePropertyValue $StageDir -Force
    $info | Add-Member -NotePropertyName source -NotePropertyValue 'ci' -Force

    # Write beside the destination and rename into place: the Terminal watches
    # this folder and reacts to the write, so a half-written marker would be read
    # as malformed. A rename is atomic enough that it never sees a partial one.
    $markerTemp = "$MarkerPath.tmp"
    $info | ConvertTo-Json | Set-Content -Path $markerTemp -Encoding UTF8
    Move-Item $markerTemp $MarkerPath -Force

    # Same artifact carries a Test-branded unpacked payload too (build.yml stages
    # both from one compile). Nothing today asks for it automatically -- Refresh-
    # TestSlot.ps1 does that on demand -- but stage it every time regardless, same
    # as the Dev half: staging is cheap and automatic, registering never is.
    $testPayloadSrc = Join-Path $temp 'test-staged'
    if (Test-Path (Join-Path $testPayloadSrc 'AppxManifest.xml')) {
        if (Test-Path $TestStageDir) { Remove-Item -Recurse -Force $TestStageDir }
        Move-Item $testPayloadSrc $TestStageDir

        # No separate marker ships for the Test half; it is the same build as the
        # Dev marker just staged, so describe it with the same commit/branch/time.
        $testInfo = $info | Select-Object commit, commitFull, branch, dirty, timestampUtc, timestamp, source
        $testInfo | Add-Member -NotePropertyName payload -NotePropertyValue $TestStageDir -Force
        $testMarkerTemp = "$TestMarkerPath.tmp"
        $testInfo | ConvertTo-Json | Set-Content -Path $testMarkerTemp -Encoding UTF8
        Move-Item $testMarkerTemp $TestMarkerPath -Force
    }
    else {
        Say 'Artifact has no test-staged payload (older CI run, or build.yml changed) -- Test half not updated.' ([ConsoleColor]::DarkYellow)
    }

    # New artifacts carry the matching helpers; old artifacts fall back to this checkout.
    Sync-SlotHelpers -SourceDirectory $PSScriptRoot
    Sync-SlotHelpers -SourceDirectory $temp

    if ($WithSymbols) { Get-CISymbols -RunDatabaseId $run.databaseId -Sha $run.headSha }

    Say "Staged $($info.commit) ($($info.branch)) built $($info.timestampUtc)" ([ConsoleColor]::Green)
    Say 'muld will offer it as an update.'
    if (Test-Path $TestStageDir) {
        Say 'mult: run Refresh-TestSlot.ps1 to pick it up.' ([ConsoleColor]::Green)
    }
}
finally {
    Remove-Item -Recurse -Force $temp -ErrorAction SilentlyContinue
}
