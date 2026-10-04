<#
.SYNOPSIS
Rejects a {StaticResource}/{ThemeResource} key that nothing defines.

.DESCRIPTION
A resource key that cannot be found is not a build error. The XAML compiler
accepts it, the package is produced, both unit suites pass, and the page that
uses it fail-fasts the moment it loads: 0xc000027b, a stowed exception in
Windows.UI.Xaml.dll, with no frame of ours on the stack.

That is what upstream #20336 did to the Personalization page on 2026-10-04. It
deleted PivotStackStyle from CommonResources.xaml once nothing of upstream's
used it; GlobalAppearance.xaml, which is the fork's, still did, and the merge
was clean. Opening Settings > Personalization killed the Dev slot.

Every key referenced from the app's XAML must either be defined by some XAML
in the tree (x:Key, or x:Name for MUX-style named resources) or be listed in
tools\XamlExternalResourceKeys.txt as one Windows or WinUI supplies. A key in
neither is reported with the files that use it. If it really is a system
resource, add it to that list; otherwise it is this bug.

.EXAMPLE
pwsh -File tools\Check-XamlResourceKeys.ps1
#>
param(
    [string]$Root = (Join-Path $PSScriptRoot '..\src\cascadia'),
    [string]$AllowList = (Join-Path $PSScriptRoot 'XamlExternalResourceKeys.txt')
)

$ErrorActionPreference = 'Stop'

$files = Get-ChildItem -Path $Root -Recurse -File -Filter *.xaml |
    Where-Object { $_.FullName -notmatch '\\(Generated Files|obj|bin)\\' }

$defined = [System.Collections.Generic.HashSet[string]]::new()
$usedBy = @{}
foreach ($file in $files) {
    $text = Get-Content -LiteralPath $file.FullName -Raw
    foreach ($m in [regex]::Matches($text, 'x:(?:Key|Name)="([^"]+)"')) {
        [void]$defined.Add($m.Groups[1].Value)
    }
    foreach ($m in [regex]::Matches($text, '\{(?:StaticResource|ThemeResource) ([^}\s]+)\}')) {
        $key = $m.Groups[1].Value
        if (-not $usedBy.ContainsKey($key)) { $usedBy[$key] = [System.Collections.Generic.SortedSet[string]]::new() }
        [void]$usedBy[$key].Add($file.Name)
    }
}

$external = [System.Collections.Generic.HashSet[string]]::new()
foreach ($line in Get-Content -LiteralPath $AllowList) {
    $key = $line.Trim()
    if ($key -and -not $key.StartsWith('#')) { [void]$external.Add($key) }
}

$missing = @($usedBy.Keys | Where-Object { -not $defined.Contains($_) -and -not $external.Contains($_) } | Sort-Object)

if ($files.Count -eq 0 -or $usedBy.Count -eq 0) {
    # A check that found nothing to check has not passed.
    Write-Error "Scanned $($files.Count) XAML files and found $($usedBy.Count) resource references under $Root; refusing to report success."
}

if ($missing.Count -gt 0) {
    foreach ($key in $missing) {
        Write-Host "Undefined resource key '$key', used by: $($usedBy[$key] -join ', ')"
    }
    Write-Error "$($missing.Count) resource key(s) are referenced but defined nowhere. Each one fail-fasts the page that uses it at load. Define it, or add it to $(Split-Path $AllowList -Leaf) if Windows or WinUI supplies it."
}

Write-Host "Checked $($usedBy.Count) resource keys referenced from $($files.Count) XAML files: all defined in the tree or supplied by the platform ($($external.Count) known external keys)."
