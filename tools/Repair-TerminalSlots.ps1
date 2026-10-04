# Finds -- and optionally clears -- "zombie" Terminal processes: a mullion.exe
# that is alive and holding a slot's single-instance identity while owning no window
# at all.
#
# Why this is worth a script of its own: the emperor owns the single-instance identity
# for its package (the window class and mutex mix in the package family name, see
# WindowEmperor::HandleCommandlineArgs). A process sitting there with zero windows
# still answers for the whole app -- every later muld/mult launch finds it, hands its
# commandline over via WM_COPYDATA and exits. The visible symptom is the worst kind:
# the Terminal simply stops opening. No window, no error, no crash, no event log
# entry, and Get-Process still shows a healthy process. It stays that way until
# someone kills it by hand.
#
# WindowEmperor::_armNoWindowWatchdog() makes the app quit itself when this happens,
# so a build carrying that fix cannot produce a lasting zombie. This script is the
# out-of-process backstop: it covers slots still running an older payload, and it is
# the fastest way to confirm the diagnosis.
#
# Detection deliberately does NOT use Process.MainWindowHandle. That property only
# finds a *visible* top-level window, so a Terminal legitimately minimised to the
# notification area reads as 0 and would be killed as a false positive. Counting
# every top-level window owned by the process -- hidden ones included -- is the
# distinction that actually matters: "a window exists but you cannot see it" is fine,
# "no window exists" is the zombie.
#
# Reports by default and changes nothing. -Force clears the Test slot; the Dev slot
# additionally needs -IncludeDev, because a windowless Dev process can still hold a
# live shell you cannot reach, and per AGENTS.md that is your call, not a script's.

[CmdletBinding()]
Param(
    [ValidateSet('Test', 'Dev', 'All')]
    [string]$Slot = 'All',
    [string]$TestPayload = 'C:\TerminalSlots\test',
    [string]$DevPayload = 'C:\TerminalSlots\dev',
    # A Terminal that is still starting legitimately has no window yet. Nothing is
    # judged until it has had this long to produce one.
    [int]$GraceSeconds = 90,
    [switch]$Force,
    [switch]$IncludeDev
)

$ErrorActionPreference = 'Stop'

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public static class TerminalSlotWindows
{
    private delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr lParam);

    [DllImport("user32.dll")]
    private static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint processId);

    // Counts every top-level window owned by the process, visible or not.
    // EnumWindows does not walk message-only (HWND_MESSAGE) windows, which is
    // exactly what we want: the emperor's message window is not a UI window and
    // must not make a windowless process look healthy.
    public static int CountFor(uint pid)
    {
        int count = 0;
        EnumWindows((hWnd, lParam) =>
        {
            uint owner;
            GetWindowThreadProcessId(hWnd, out owner);
            if (owner == pid) { count++; }
            return true;
        }, IntPtr.Zero);
        return count;
    }
}
'@

function Get-SlotProcess {
    Param([string]$PayloadDir)

    if (-not $PayloadDir) { return @() }

    # Identify by executable path, never by process name: every Terminal built from
    # this repo -- test, dev, and whatever is hosting your session -- is called
    # mullion.exe (or WindowsTerminal.exe, from before the rename).
    @(Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path -like "$PayloadDir\*" })
}

function Test-Slot {
    Param(
        [string]$Name,
        [string]$PayloadDir,
        [bool]$MayClear
    )

    $found = @()
    foreach ($proc in Get-SlotProcess -PayloadDir $PayloadDir) {
        $windows = [TerminalSlotWindows]::CountFor([uint32]$proc.Id)
        $age = (Get-Date) - $proc.StartTime

        if ($windows -gt 0) {
            Write-Host ("   {0,-5} pid {1,-7} {2} window(s) -- healthy" -f $Name, $proc.Id, $windows) -ForegroundColor DarkGray
            continue
        }

        if ($age.TotalSeconds -lt $GraceSeconds) {
            Write-Host ("   {0,-5} pid {1,-7} no window yet, but only {2:N0}s old -- still starting, left alone" -f $Name, $proc.Id, $age.TotalSeconds) -ForegroundColor DarkGray
            continue
        }

        Write-Host ("   {0,-5} pid {1,-7} NO WINDOW, up {2:hh\:mm\:ss} -- zombie" -f $Name, $proc.Id, $age) -ForegroundColor Red

        # Say what dies with it, so a decision about the Dev slot is an informed one.
        $kids = @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($proc.Id)" -ErrorAction SilentlyContinue)
        foreach ($kid in $kids) {
            Write-Host ("        holds: {0} (pid {1})" -f $kid.Name, $kid.ProcessId) -ForegroundColor DarkYellow
        }

        $found += [pscustomobject]@{ Slot = $Name; Process = $proc; Children = $kids.Count }
    }

    if (-not $found) { return 0 }

    if (-not $Force) {
        Write-Host ("   {0}: {1} zombie(s) found. Re-run with -Force to clear." -f $Name, $found.Count) -ForegroundColor Yellow
        return $found.Count
    }

    if (-not $MayClear) {
        Write-Host ("   {0}: {1} zombie(s) found. Add -IncludeDev to clear the Dev slot as well." -f $Name, $found.Count) -ForegroundColor Yellow
        return $found.Count
    }

    foreach ($entry in $found) {
        $proc = $entry.Process
        # Re-verify the path immediately before killing. The PID is only as good as
        # the moment it was read, and the cost of getting this wrong is someone's
        # live session.
        $live = Get-Process -Id $proc.Id -ErrorAction SilentlyContinue
        if (-not $live) { continue }
        if ($live.Path -notlike "$PayloadDir\*") {
            Write-Host ("   refusing pid {0}: path is now {1}" -f $live.Id, $live.Path) -ForegroundColor Red
            continue
        }
        Stop-Process -Id $live.Id -Force -ErrorAction SilentlyContinue
        Write-Host ("   cleared {0} pid {1}" -f $entry.Slot, $live.Id) -ForegroundColor Green
    }

    return 0
}

Write-Host '== checking for windowless Terminal processes ==' -ForegroundColor Cyan

$remaining = 0
if ($Slot -in @('Test', 'All')) {
    $remaining += Test-Slot -Name 'Test' -PayloadDir $TestPayload -MayClear $true
}
if ($Slot -in @('Dev', 'All')) {
    # Dev is production. Clearing it is opt-in even under -Force.
    $remaining += Test-Slot -Name 'Dev' -PayloadDir $DevPayload -MayClear ([bool]$IncludeDev)
}

if ($remaining -gt 0) {
    Write-Host "$remaining zombie(s) still present." -ForegroundColor Yellow
    exit 1
}

Write-Host 'No windowless Terminal processes.' -ForegroundColor Green
exit 0
