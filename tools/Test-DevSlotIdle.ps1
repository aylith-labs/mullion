# Read-only check for the narrow auto-promote exception in CLAUDE.md ("The two slots").
#
# Answers one question: is it safe to run Promote-DevSlot.ps1 without asking first?
# Safe means either nothing is running under the Dev payload, or exactly one window
# is running with exactly one tab/pane whose foreground process is a session
# multiplexer (shefrd, herdr, tmux, screen, zellij) rather than a raw shell -- in
# which case the actual work lives in the multiplexer's own persistent server, not
# the window, and closing the window loses nothing.
#
# This is a heuristic, not a proof. Process-tree shape is an approximation of tab
# and pane count -- a multiplexer client can fork helper processes that look like
# extra leaves, and correlating a specific WSL pty back to a specific Windows-side
# wsl.exe process is inexact. Ambiguous results MUST be treated as unsafe (exit 1)
# rather than guessed safe: this script fails closed on purpose.
#
# Never closes, kills, or promotes anything itself. It only reports.

[CmdletBinding()]
Param(
    [string]$DevPayload = 'C:\TerminalSlots\dev',
    [string]$Distro = 'Ubuntu',
    [string[]]$KnownMultiplexers = @('shefrd', 'herdr', 'tmux', 'screen', 'zellij')
)

$ErrorActionPreference = 'Stop'

function Get-ProcessTree {
    Param([int]$RootProcessId)
    $all = Get-CimInstance Win32_Process
    $byParent = @{}
    foreach ($p in $all) {
        $key = [string]$p.ParentProcessId
        if (-not $byParent.ContainsKey($key)) { $byParent[$key] = @() }
        $byParent[$key] += $p
    }
    $result = [System.Collections.Generic.List[object]]::new()
    $queue = [System.Collections.Generic.Queue[int]]::new()
    $queue.Enqueue($RootProcessId)
    while ($queue.Count -gt 0) {
        $current = $queue.Dequeue()
        $key = [string]$current
        if ($byParent.ContainsKey($key)) {
            foreach ($child in $byParent[$key]) {
                $result.Add($child)
                $queue.Enqueue($child.ProcessId)
            }
        }
    }
    return $result
}

$devWindows = @(Get-CimInstance Win32_Process -Filter "Name='mullion.exe' OR Name='WindowsTerminal.exe'" |
    Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($DevPayload, [StringComparison]::OrdinalIgnoreCase) })

if ($devWindows.Count -eq 0) {
    Write-Output 'SAFE: no mullion.exe running under the Dev payload'
    exit 0
}

if ($devWindows.Count -gt 1) {
    Write-Output "NOT SAFE: $($devWindows.Count) Dev windows running (need exactly 0 or 1)"
    exit 1
}

$window = $devWindows[0]
$tree = Get-ProcessTree -RootProcessId $window.ProcessId

# Leaves under a WT window are its shell hosts: one wsl.exe (or one native shell)
# per tab/pane. Anything else hanging off the tree (OpenConsole.exe conpty hosts,
# wslhost.exe, conhost.exe) is plumbing, not a session.
$leafNames = @('wsl.exe', 'cmd.exe', 'powershell.exe', 'pwsh.exe')
$leaves = @($tree | Where-Object { $leafNames -contains $_.Name })

if ($leaves.Count -eq 0) {
    Write-Output "NOT SAFE: found a Dev window (pid $($window.ProcessId)) but no recognizable shell host under it -- ambiguous, failing closed"
    exit 1
}

if ($leaves.Count -gt 1) {
    Write-Output "NOT SAFE: $($leaves.Count) shell hosts under the one Dev window (need exactly 1) -- likely more than one tab/pane"
    exit 1
}

$leaf = $leaves[0]

if ($leaf.Name -ne 'wsl.exe') {
    # A native cmd/powershell leaf: check its own descendants for a known multiplexer.
    $descendants = Get-ProcessTree -RootProcessId $leaf.ProcessId
    $match = $descendants | Where-Object { $KnownMultiplexers -contains ([System.IO.Path]::GetFileNameWithoutExtension($_.Name)) } | Select-Object -First 1
    if ($match) {
        Write-Output "SAFE: single native session, running $($match.Name)"
        exit 0
    }
    Write-Output "NOT SAFE: single native shell host (pid $($leaf.ProcessId)), no recognized multiplexer found among its descendants"
    exit 1
}

# wsl.exe leaf: the multiplexer runs inside the WSL VM, invisible to the Windows
# process tree. Ask WSL directly whether a known multiplexer client is attached,
# and require exactly one match system-wide -- more than one leaves which pty
# belongs to this tab ambiguous, so that also fails closed.
$pattern = ($KnownMultiplexers -join '|')
$psOutput = & wsl.exe -d $Distro -- bash -c "ps -eo comm | grep -E '^($pattern)$' | sort | uniq -c" 2>$null

if (-not $psOutput) {
    Write-Output "NOT SAFE: single wsl.exe session (pid $($leaf.ProcessId)) but no known multiplexer process found inside $Distro"
    exit 1
}

$lines = @($psOutput -split "`n" | Where-Object { $_.Trim() })
$totalMatches = ($lines | ForEach-Object { [int]($_.Trim() -split '\s+')[0] } | Measure-Object -Sum).Sum

if ($totalMatches -ne 1) {
    Write-Output "NOT SAFE: found $totalMatches multiplexer-like processes inside ${Distro}: $($lines -join '; ') -- cannot confirm this is the one session under our one wsl.exe leaf, failing closed"
    exit 1
}

Write-Output "SAFE: single wsl.exe session (pid $($leaf.ProcessId)), exactly one multiplexer process running inside ${Distro}: $($lines[0].Trim())"
exit 0
