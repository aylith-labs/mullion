# Emits a C++ header describing which commit this build came from and when it
# was built, on stdout. The caller (build/rules/GenerateBuildInfo.proj) captures
# the output and writes the file, mirroring Generate-FeatureStagingHeader.ps1.
#
# Everything degrades to a placeholder rather than failing the build: a source
# archive with no .git, or a machine with no git on PATH, still compiles.
[CmdletBinding()]
Param(
    [string]$Branding = "Dev"
)

$ErrorActionPreference = 'Continue'

function Invoke-Git {
    Param([string[]]$Arguments, [string]$Fallback = 'unknown')
    try {
        $out = & git.exe @Arguments 2>$null
        if ($LASTEXITCODE -ne 0 -or [string]::IsNullOrWhiteSpace($out)) { return $Fallback }
        return ($out | Select-Object -First 1).Trim()
    } catch {
        return $Fallback
    }
}

$commitShort = Invoke-Git @('rev-parse', '--short=9', 'HEAD')
$commitFull  = Invoke-Git @('rev-parse', 'HEAD')
$branch      = Invoke-Git @('rev-parse', '--abbrev-ref', 'HEAD')

# A non-empty porcelain listing means the tree had uncommitted changes when this
# was built, so the commit hash alone does not identify the binary.
$status = ''
try {
    $status = (& git.exe status --porcelain 2>$null) -join ''
} catch {
    $status = ''
}
$dirty = if ([string]::IsNullOrWhiteSpace($status)) { 0 } else { 1 }

$now      = [DateTimeOffset]::UtcNow
$unix     = $now.ToUnixTimeSeconds()
$readable = $now.UtcDateTime.ToString('yyyy-MM-dd HH:mm:ss') + ' UTC'

# Escaping: hashes and branch names can contain characters that are awkward in a
# C++ string literal (a branch is only barred from a small set). Backslash and
# double quote are the two that would break the literal.
function ConvertTo-CppLiteral {
    Param([string]$Value)
    return $Value.Replace('\', '\\').Replace('"', '\"')
}

@"
// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.
//
// GENERATED FILE -- DO NOT EDIT.
// Produced by tools/Generate-BuildInfoHeader.ps1 during the build.

#pragma once

#define TERMINAL_BUILD_COMMIT L"$(ConvertTo-CppLiteral $commitShort)"
#define TERMINAL_BUILD_COMMIT_FULL L"$(ConvertTo-CppLiteral $commitFull)"
#define TERMINAL_BUILD_BRANCH L"$(ConvertTo-CppLiteral $branch)"
#define TERMINAL_BUILD_BRANDING L"$(ConvertTo-CppLiteral $Branding)"

// 1 when the working tree had uncommitted changes at build time.
#define TERMINAL_BUILD_DIRTY $dirty

// Seconds since the Unix epoch, UTC, at the moment this header was generated.
#define TERMINAL_BUILD_TIMESTAMP ${unix}LL
#define TERMINAL_BUILD_TIMESTAMP_STRING L"$readable"
"@

# Release notes for the About dialog: one entry per CI build of main, written by
# tools\Write-ReleaseNotes.ps1 into the file MULLION_RELEASE_NOTES names. Compiled
# in rather than packaged, so there is no asset to forget to package. A build
# without that file (any local one) gets an empty list, never a failure.
#
# Windows PowerShell 5.1 runs this (GenerateBuildInfo.proj), so no ?? or ternaries.
function ConvertTo-CppWideLiteral {
    Param([string]$Value)
    $sb = [System.Text.StringBuilder]::new('L"')
    foreach ($ch in $Value.ToCharArray()) {
        $code = [int]$ch
        if ($ch -eq '\') { [void]$sb.Append('\\') }
        elseif ($ch -eq '"') { [void]$sb.Append('\"') }
        elseif ($code -ge 0x20 -and $code -le 0x7E) { [void]$sb.Append($ch) }
        else {
            # A hex escape runs on through any hex digit that follows it, so close
            # the literal after each one; adjacent literals concatenate.
            [void]$sb.Append(('\x{0:X4}" L"' -f $code))
        }
    }
    [void]$sb.Append('"')
    return $sb.ToString()
}

$notes = @()
if ($env:MULLION_RELEASE_NOTES -and (Test-Path -LiteralPath $env:MULLION_RELEASE_NOTES)) {
    try {
        # 5.1's ConvertFrom-Json emits a top-level array as ONE object; @() around it
        # would make a one-element list holding the whole array. foreach unrolls it.
        $parsed = Get-Content -LiteralPath $env:MULLION_RELEASE_NOTES -Raw -Encoding UTF8 | ConvertFrom-Json
        foreach ($entry in $parsed) { $notes += $entry }
    }
    catch {
        $notes = @()
    }
}

''
'// Release notes: one entry per CI build of main, newest first (tools/Write-ReleaseNotes.ps1).'
'struct TerminalReleaseNoteCommit { const wchar_t* Sha; const wchar_t* Subject; };'
'struct TerminalReleaseNoteBuild { const wchar_t* Sha; long long BuiltAt; int RunNumber; const TerminalReleaseNoteCommit* Commits; int CommitCount; };'
$rows = @()
for ($i = 0; $i -lt $notes.Count; $i++) {
    $build = $notes[$i]
    $commits = @($build.commits)
    if ($commits.Count -gt 0) {
        "inline constexpr TerminalReleaseNoteCommit TerminalReleaseNoteCommits_$i[] = {"
        foreach ($commit in $commits) {
            "    { $(ConvertTo-CppWideLiteral ([string]$commit.sha)), $(ConvertTo-CppWideLiteral ([string]$commit.subject)) },"
        }
        '};'
        $list = "TerminalReleaseNoteCommits_$i"
    }
    else {
        $list = 'nullptr'
    }
    $rows += "    { $(ConvertTo-CppWideLiteral ([string]$build.sha)), $([long]$build.builtAt)LL, $([int]$build.run), $list, $($commits.Count) },"
}
if ($rows.Count -gt 0) {
    'inline constexpr TerminalReleaseNoteBuild TerminalReleaseNoteBuildArray[] = {'
    $rows
    '};'
    'inline constexpr const TerminalReleaseNoteBuild* TerminalReleaseNoteBuildList = TerminalReleaseNoteBuildArray;'
}
else {
    'inline constexpr const TerminalReleaseNoteBuild* TerminalReleaseNoteBuildList = nullptr;'
}
"inline constexpr int TerminalReleaseNoteBuildCount = $($rows.Count);"
