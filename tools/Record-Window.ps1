<#
.SYNOPSIS
    Records a window to a numbered PNG sequence, without raising or focusing it.

.DESCRIPTION
    The moving parts of this fork are worth showing moving: a pane coming back
    with what it was running, a card flipping to stay inside the window. This
    grabs frames the same way Capture-Window.ps1 grabs stills -- PrintWindow,
    which reads a window's own pixels while it sits behind other windows and
    never takes focus.

    Frames are written as frame-00000.png upward, which is what ffmpeg's image
    sequence demuxer expects.

    A window that does not exist yet, or has gone away mid-recording, is not an
    error: the point of most of these recordings is a Terminal restarting, so
    the process id is looked up fresh on every frame and missing frames simply
    repeat the last good one. That keeps the sequence at a constant rate, which
    is what makes the output play at real speed.

.PARAMETER MatchPath
    Only capture windows whose process image path starts with this. Guards
    against recording the wrong Terminal: there is more than one running, and
    only the disposable slot may be driven.

.PARAMETER Seconds
    How long to record.

.PARAMETER Fps
    Frames per second. 10 is plenty for terminal output and keeps files small.

.PARAMETER OutDir
    Directory for the frames. Created if missing, emptied if it already has some.

.EXAMPLE
    ./Record-Window.ps1 -MatchPath 'C:\TerminalSlots\test\' -Seconds 20 -OutDir frames
#>
[CmdletBinding()]
param(
    [string]$MatchPath = 'C:\TerminalSlots\test\',
    [int]$Seconds = 15,
    [int]$Fps = 10,
    [int]$WindowIndex = 0,
    [Parameter(Mandatory)][string]$OutDir
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public class RecNative {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern int GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static List<IntPtr> WindowsFor(uint pid) {
        var found = new List<IntPtr>();
        EnumWindows((h, l) => {
            uint owner;
            GetWindowThreadProcessId(h, out owner);
            if (owner == pid && IsWindowVisible(h) && GetWindowTextLength(h) > 0) {
                RECT r; GetWindowRect(h, out r);
                if ((r.R - r.L) > 100 && (r.B - r.T) > 100) { found.Add(h); }
            }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@

if (Test-Path $OutDir) { Remove-Item (Join-Path $OutDir 'frame-*.png') -Force -ErrorAction SilentlyContinue }
else { New-Item -ItemType Directory -Force $OutDir | Out-Null }

$intervalMs = [int](1000 / $Fps)
$total = $Seconds * $Fps
$lastGood = $null
$captured = 0
$blanks = 0

for ($i = 0; $i -lt $total; $i++) {
    $frameStart = [Environment]::TickCount
    $path = Join-Path $OutDir ('frame-{0:D5}.png' -f $i)
    $done = $false

    # Resolved every frame: the window this is following may be a Terminal that
    # is in the middle of restarting, so its process id is not stable.
    $proc = Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($MatchPath, [StringComparison]::OrdinalIgnoreCase) } |
        Select-Object -First 1

    if ($proc) {
        $handles = [RecNative]::WindowsFor([uint32]$proc.Id)
        if ($handles.Count -gt $WindowIndex) {
            $hwnd = $handles[$WindowIndex]
            $r = New-Object RecNative+RECT
            [void][RecNative]::GetWindowRect($hwnd, [ref]$r)
            $w = $r.R - $r.L; $h = $r.B - $r.T
            if ($w -gt 100 -and $h -gt 100) {
                $bmp = New-Object System.Drawing.Bitmap $w, $h
                $g = [System.Drawing.Graphics]::FromImage($bmp)
                try {
                    $hdc = $g.GetHdc()
                    try { $ok = [RecNative]::PrintWindow($hwnd, $hdc, 2) } finally { $g.ReleaseHdc($hdc) }
                    if ($ok) {
                        $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
                        $lastGood = $path
                        $captured++
                        $done = $true
                    }
                }
                finally { $g.Dispose(); $bmp.Dispose() }
            }
        }
    }

    # Hold the previous frame so the sequence stays at a constant rate; without
    # this the gap while the Terminal restarts would play back as a speed-up.
    if (-not $done) {
        $blanks++
        if ($lastGood) { Copy-Item $lastGood $path -Force }
    }

    $spent = [Environment]::TickCount - $frameStart
    $wait = $intervalMs - $spent
    if ($wait -gt 0) { Start-Sleep -Milliseconds $wait }
}

"frames: $total  captured: $captured  held: $blanks  ->  $OutDir"
