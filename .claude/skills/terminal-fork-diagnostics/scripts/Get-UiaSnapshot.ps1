# Report what the Test slot's window actually contains, from the UI Automation
# tree: control type, name, and bounding rectangle.
#
# This is the verification tool for UI work in this repo, in preference to a
# screenshot. PrintWindow cannot capture DirectComposition content in XAML
# Islands, so it returns the terminal's swap chain and none of the XAML chrome -
# which reads as "nothing rendered" and is not. Geometry from UIA is proof:
# a strip 300 wide and 1959 tall IS a vertical sidebar, whatever a screenshot
# says, and a TabItem count of 0 after an action IS lost tabs.
#
# Windows PowerShell 5.1 only. UIAutomationClient is not available in pwsh 7.
#
#   powershell.exe -NoProfile -File .\Get-UiaSnapshot.ps1
#   powershell.exe -NoProfile -File .\Get-UiaSnapshot.ps1 -CountOnly
#   powershell.exe -NoProfile -File .\Get-UiaSnapshot.ps1 -ControlTypes Button,TabItem
[CmdletBinding()]
Param(
    [string[]]$ControlTypes = @('TabItem', 'Tab', 'List', 'ListItem', 'Button', 'SplitButton', 'Text', 'Edit'),
    # Just the per-type counts. This is the form to use for before/after
    # comparisons - a count is unambiguous where a description is not.
    [switch]$CountOnly,
    # Anything whose top edge is within this many pixels of the window's counts
    # as titlebar rather than body.
    [int]$TitlebarDepth = 60,
    [string]$SlotRoot = 'C:\TerminalSlots'
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes

$testPath = Join-Path $SlotRoot 'test'
$w = Get-Process -Name 'mullion', 'WindowsTerminal' -ErrorAction SilentlyContinue |
     Where-Object { $_.Path -and $_.Path -like "$testPath\*" -and $_.MainWindowHandle -ne 0 } |
     Select-Object -First 1
if (-not $w) { throw 'no mult window - launch the Test slot first' }

$root = [System.Windows.Automation.AutomationElement]::FromHandle($w.MainWindowHandle)
$wr = $root.Current.BoundingRectangle
Write-Host ("window pid {0}  x={1:N0} y={2:N0} w={3:N0} h={4:N0}" -f $w.Id, $wr.X, $wr.Y, $wr.Width, $wr.Height)

foreach ($ct in $ControlTypes) {
    $type = [System.Windows.Automation.ControlType]::$ct
    if (-not $type) { Write-Warning "unknown control type '$ct'"; continue }

    $cond = New-Object System.Windows.Automation.PropertyCondition(
        [System.Windows.Automation.AutomationElement]::ControlTypeProperty, $type)
    $found = @($root.FindAll([System.Windows.Automation.TreeScope]::Descendants, $cond))

    # Offscreen and zero-width elements are counted separately: a control that is
    # present but collapsed is a different bug from one that is absent.
    $visible = @($found | Where-Object { -not $_.Current.IsOffscreen -and $_.Current.BoundingRectangle.Width -gt 0 })
    Write-Host ("{0,-12} total={1,-3} visible={2}" -f $ct, $found.Count, $visible.Count)

    if ($CountOnly) { continue }

    foreach ($e in $visible) {
        $r = $e.Current.BoundingRectangle
        $where = if ($r.Y -lt ($wr.Y + $TitlebarDepth)) { 'TITLEBAR' } else { 'body' }
        # Patterns matter: a name alone will match a Text element that merely
        # labels the control you wanted, so record what can actually be driven.
        $pats = @()
        foreach ($p in $e.GetSupportedPatterns()) { $pats += ($p.ProgrammaticName -replace 'PatternIdentifiers.Pattern', '') }
        Write-Host ("   {0,-9} '{1}' id='{2}' x={3:N0} y={4:N0} w={5:N0} h={6:N0} [{7}]" -f
            $where, $e.Current.Name, $e.Current.AutomationId, $r.X, $r.Y, $r.Width, $r.Height, ($pats -join ','))
    }
}

Write-Host ''
Write-Host 'Note: a SettingsCard cannot be invoked over UIA, so do not try. Its peer'
Write-Host 'derives from ButtonBaseAutomationPeer, which supplies no IInvokeProvider,'
Write-Host 'and ButtonBase exposes no way to raise Click from outside - while Click is'
Write-Host 'what every card in the editor is wired to. It no longer claims the pattern'
Write-Host 'either, so a client gets an honest "not available" rather than a failed QI.'
Write-Host 'To drive a card, click the centre of the rectangle printed above with'
Write-Host 'AgentDriver.exe, honouring the idle rules in the global CLAUDE.md.'
