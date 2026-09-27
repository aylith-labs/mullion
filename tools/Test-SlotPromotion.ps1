# Exercises the real promotion script with fake packages and launches, never a live slot.
[CmdletBinding()]
param([switch]$Worker, [string]$FixtureRoot, [switch]$FailRotation, [switch]$FailRegistration)
$ErrorActionPreference = 'Stop'
$promotionScript = Join-Path $PSScriptRoot 'Promote-DevSlot.ps1'

if ($Worker) {
    $global:promotionTestFixture = [IO.Path]::GetFullPath($FixtureRoot)
    function Get-Process { param($Name) return @() }
    function Get-AppxPackage {
        param($Name)
        return [pscustomobject]@{ InstallLocation = (Join-Path $global:promotionTestFixture 'dev'); PackageFullName = 'FakeTerminal' }
    }
    function Remove-AppxPackage { param($Package, [switch]$PreserveApplicationData) }
    function Add-AppxPackage {
        param($Path, [switch]$Register)
        if ($FailRegistration) { throw 'Injected registration failure' }
        Set-Content -LiteralPath (Join-Path $global:promotionTestFixture 'registered') -Value 'yes'
    }
    function Start-Process {
        param($FilePath)
        if ($FilePath -ne 'muld.exe') { throw 'Unexpected launch' }
        Set-Content -LiteralPath (Join-Path $global:promotionTestFixture 'relaunched') -Value 'yes'
    }
    function Remove-Item {
        [CmdletBinding()]
        param([string]$Path, [string]$LiteralPath, [switch]$Force, [switch]$Recurse)
        $target = if ($LiteralPath) { $LiteralPath } else { $Path }
        $resolved = [IO.Path]::GetFullPath($target)
        if (-not $resolved.StartsWith($global:promotionTestFixture + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Deletion escaped fixture' }
        if ($FailRotation -and $resolved -eq (Join-Path $global:promotionTestFixture 'dev.rollback.2')) { throw 'Injected locked rollback' }
        Microsoft.PowerShell.Management\Remove-Item -LiteralPath $resolved -Force:$Force -Recurse:$Recurse
    }
    & $promotionScript -Payload (Join-Path $global:promotionTestFixture 'dev') -Staged (Join-Path $global:promotionTestFixture 'staged') -Marker (Join-Path $global:promotionTestFixture 'pending.json') -Relaunch
    exit $LASTEXITCODE
}

function Assert-True($condition, [string]$message) { if (-not $condition) { throw $message } }
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('terminal-promotion-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
function New-Payload([string]$path, [string]$label) {
    New-Item -ItemType Directory -Path $path -Force | Out-Null
    Set-Content -LiteralPath (Join-Path $path 'AppxManifest.xml') -Value '<Package/>'
    Set-Content -LiteralPath (Join-Path $path 'build.txt') -Value $label
}
function Invoke-Promotion([string]$root, [string]$failure = '') {
    Set-Content -LiteralPath (Join-Path $root 'pending.json') -Value '{}'
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $start.Arguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -Worker -FixtureRoot "{1}" {2}' -f $PSCommandPath, $root, $failure
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.EnvironmentVariables['LOCALAPPDATA'] = Join-Path $root 'appdata'
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Fixture timed out' }
        $diagnostic = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $expected = if ($failure -eq '-FailRegistration') { 1 } else { 0 }
        Assert-True ($process.ExitCode -eq $expected) "Promotion exit $($process.ExitCode): $diagnostic"
        Assert-True (Test-Path -LiteralPath (Join-Path $root 'relaunched')) 'Promotion did not relaunch'
    }
    finally { $process.Dispose() }
}
try {
    $repeat = Join-Path $testRoot 'repeat'
    New-Payload (Join-Path $repeat 'dev') '0'
    for ($build = 1; $build -le 5; $build++) {
        New-Payload (Join-Path $repeat 'staged') ([string]$build)
        Invoke-Promotion $repeat
        Assert-True ((Get-Content -LiteralPath (Join-Path $repeat 'dev/build.txt')) -eq [string]$build) 'Wrong promoted build'
        Assert-True ((Get-Content -LiteralPath (Join-Path $repeat 'dev.rollback.1/build.txt')) -eq [string]($build - 1)) 'Wrong newest rollback'
        if ($build -ge 2) { Assert-True ((Get-Content -LiteralPath (Join-Path $repeat 'dev.rollback.2/build.txt')) -eq [string]($build - 2)) 'Wrong oldest rollback' }
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $repeat 'dev.rollback.11'))) 'Format expression created rollback.11'
        Assert-True (-not (Test-Path -LiteralPath (Join-Path $repeat 'pending.json'))) 'Successful promotion left its marker'
        Remove-Item -LiteralPath (Join-Path $repeat 'relaunched')
    }
    $locked = Join-Path $testRoot 'locked'
    foreach ($part in 'dev', 'staged', 'dev.rollback.1', 'dev.rollback.2') { New-Payload (Join-Path $locked $part) $part }
    Invoke-Promotion $locked '-FailRotation'
    Assert-True (Test-Path -LiteralPath (Join-Path $locked 'dev.previous/build.txt')) 'Failed rotation lost previous payload'
    Assert-True (-not (Test-Path -LiteralPath (Join-Path $locked 'pending.json'))) 'Cleanup failure left successful promotion pending'
    $failed = Join-Path $testRoot 'failed'
    New-Payload (Join-Path $failed 'dev') 'old'
    New-Payload (Join-Path $failed 'staged') 'new'
    Invoke-Promotion $failed '-FailRegistration'
    Assert-True (Test-Path -LiteralPath (Join-Path $failed 'pending.json')) 'Registration failure cleared marker'

    # Load only this function's AST; never execute the fetcher or contact GitHub.
    $tokens = $null; $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile((Join-Path $PSScriptRoot 'Fetch-CIBuild.ps1'), [ref]$tokens, [ref]$parseErrors)
    Assert-True ($parseErrors.Count -eq 0) 'Fetcher has parse errors'
    $sync = $ast.Find({ param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Sync-SlotHelpers' }, $true)
    . ([scriptblock]::Create($sync.Extent.Text))
    $SlotRoot = Join-Path $testRoot 'helpers-installed'
    $helperSource = Join-Path $testRoot 'helpers-source'
    New-Item -ItemType Directory -Path $SlotRoot, $helperSource | Out-Null
    Set-Content -LiteralPath (Join-Path $SlotRoot 'Promote-DevSlot.ps1') -Value 'old'
    Set-Content -LiteralPath (Join-Path $helperSource 'Promote-DevSlot.ps1') -Value 'new'
    Sync-SlotHelpers $helperSource
    Assert-True ((Get-Content -LiteralPath (Join-Path $SlotRoot 'Promote-DevSlot.ps1')) -eq 'new') 'Installed helper stayed stale'
    Sync-SlotHelpers $SlotRoot
    Assert-True ((Get-Content -LiteralPath (Join-Path $SlotRoot 'Promote-DevSlot.ps1')) -eq 'new') 'Self-copy damaged helper'
    Write-Host 'Passed: five successive promotions, rollback failure, registration failure, helper refresh and self-copy.'
}
finally {
    $resolvedTestRoot = [IO.Path]::GetFullPath($testRoot)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    if (-not $resolvedTestRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase) -or (Split-Path -Leaf $resolvedTestRoot) -notlike 'terminal-promotion-test-*') { throw 'Unsafe fixture cleanup path' }
    Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force
}
