<#
.SYNOPSIS
Writes the release notes the About dialog shows: one entry per CI build of main.

.DESCRIPTION
Mullion has no versions of its own. The package version is a fixed 0.0.1.0 and
every tag in the repository is upstream's, so a "version" here is a successful
build of main, which is the only thing that ever reaches a slot. Each entry is
that build's commit, when it finished, its run number, and the commits it
brought: the first-parent history since the previous successful build, so an
upstream merge reads as its one merge commit rather than the dozens it carried.

The current build is the newest entry, dated now. Earlier ones come from the
workflow's own run history (actions: read). Runs that rebuilt a commit an
earlier entry already covers are folded into it.

Writes JSON to -OutFile and degrades rather than failing: no token, no API, or
a range outside the fetched history only shortens the notes. The build-info
header generator reads the file named by MULLION_RELEASE_NOTES and compiles
an empty list when there is none, so a local or offline build still builds.

.EXAMPLE
pwsh -File tools\Write-ReleaseNotes.ps1 -OutFile $env:RUNNER_TEMP\release-notes.json
#>
param(
    [Parameter(Mandatory)] [string]$OutFile,
    [int]$MaxBuilds = 30
)

$ErrorActionPreference = 'Continue'

$repo = $env:GITHUB_REPOSITORY
$head = (& git rev-parse HEAD 2>$null)
if (-not $head) { $head = $env:GITHUB_SHA }

$builds = [System.Collections.Generic.List[object]]::new()
$builds.Add([pscustomobject]@{
        sha     = "$head".Trim()
        builtAt = [DateTimeOffset]::UtcNow.ToUnixTimeSeconds()
        run     = [int]($env:GITHUB_RUN_NUMBER ?? 0)
    })

if ($repo) {
    try {
        $json = & gh api "repos/$repo/actions/workflows/build.yml/runs?branch=main&status=success&per_page=100" 2>$null
        if ($LASTEXITCODE -eq 0 -and $json) {
            $seen = [System.Collections.Generic.HashSet[string]]::new([string[]]@($builds[0].sha))
            foreach ($run in ($json | ConvertFrom-Json).workflow_runs) {
                if ($builds.Count -gt $MaxBuilds) { break }
                if (-not $seen.Add($run.head_sha)) { continue }
                $builds.Add([pscustomobject]@{
                        sha     = $run.head_sha
                        builtAt = ([DateTimeOffset]::Parse($run.updated_at)).ToUnixTimeSeconds()
                        run     = [int]$run.run_number
                    })
            }
        }
        else {
            Write-Warning "Could not list earlier builds; the release notes will hold this build only."
        }
    }
    catch {
        Write-Warning "Could not list earlier builds: $_"
    }
}

# The extra, oldest build only bounds the range of the one before it.
$entries = @()
for ($i = 0; $i -lt $builds.Count -and $i -lt $MaxBuilds; $i++) {
    $build = $builds[$i]
    $range = if ($i + 1 -lt $builds.Count) { "$($builds[$i + 1].sha)..$($build.sha)" } else { $null }
    $commits = @()
    if ($range) {
        $log = & git log --first-parent --no-decorate "--format=%h%x1f%s" $range 2>$null
        if ($LASTEXITCODE -eq 0) {
            foreach ($line in $log) {
                $parts = "$line".Split([char]0x1f, 2)
                if ($parts.Count -eq 2) { $commits += [pscustomobject]@{ sha = $parts[0]; subject = $parts[1] } }
            }
        }
    }
    if ($i -gt 0 -and $commits.Count -eq 0 -and $range) {
        # A build that brought nothing new (a re-run) says nothing worth a row.
        continue
    }
    $entries += [pscustomobject]@{
        sha     = (& git rev-parse --short=9 $build.sha 2>$null) ?? $build.sha.Substring(0, 9)
        builtAt = $build.builtAt
        run     = $build.run
        commits = $commits
    }
}

$dir = Split-Path -Parent $OutFile
if ($dir) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
ConvertTo-Json -InputObject @($entries) -Depth 5 | Set-Content -LiteralPath $OutFile -Encoding utf8
Write-Host "Release notes: $($entries.Count) builds, $(($entries | ForEach-Object { $_.commits.Count } | Measure-Object -Sum).Sum) commits -> $OutFile"
