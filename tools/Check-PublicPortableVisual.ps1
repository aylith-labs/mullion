$ErrorActionPreference = 'Stop'
if ($env:GITHUB_ACTIONS -ne 'true' -or $env:RUNNER_OS -ne 'Windows') { throw 'Dedicated Windows Actions runner required' }
$root = Join-Path $env:RUNNER_TEMP ('mullion-visual-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $root | Out-Null
$zip = Join-Path $root 'customer.zip'
Invoke-WebRequest 'https://github.com/aylith-labs/mullion/releases/download/portable-8fc3229/mullion-windows-x64-portable.zip' -OutFile $zip
if ((Get-FileHash $zip).Hash.ToLower() -ne '730300488a32d2ef1479b0e1a6687a2a3f5c14bfe3df1d9ad86c4f67b3d27b22') { throw 'Public customer archive identity differs' }
$fresh = Join-Path $root 'customer'
Expand-Archive $zip -DestinationPath $fresh
if (@(Get-ChildItem $fresh -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }).Count) { throw 'Unexpected customer link' }
foreach ($name in 'mullion.exe','.portable','resources.pri','Microsoft.UI.Xaml.dll') { if (-not (Test-Path (Join-Path $fresh $name))) { throw 'Customer layout incomplete' } }
$settings = Join-Path $fresh 'settings'
New-Item -ItemType Directory $settings | Out-Null
@{defaultProfile='{2534e893-72cf-4c6a-98e0-071479751f66}'; profiles=@{list=@(@{guid='{2534e893-72cf-4c6a-98e0-071479751f66}';name='Fixture shell';commandline='cmd.exe';startingDirectory=$fresh})};keybindings=@(@{id='Terminal.OpenAboutDialog';keys='ctrl+shift+f12'})} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $settings 'settings.json') -Encoding utf8
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Windows.Forms,System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class OwnedWindow {
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
 [StructLayout(LayoutKind.Sequential)] public struct Rect {public int Left,Top,Right,Bottom;}
}
'@
function Capture-Owned([IntPtr]$handle, [string]$path) {
 $rect = [OwnedWindow+Rect]::new()
 if (-not [OwnedWindow]::GetWindowRect($handle,[ref]$rect)) { throw 'Cannot read owned window bounds' }
 $bitmap = [Drawing.Bitmap]::new($rect.Right-$rect.Left,$rect.Bottom-$rect.Top)
 $graphics = [Drawing.Graphics]::FromImage($bitmap)
 $dc = $graphics.GetHdc()
 try { if (-not [OwnedWindow]::PrintWindow($handle,$dc,2)) { throw 'Owned window capture failed' } } finally { $graphics.ReleaseHdc($dc);$graphics.Dispose() }
 try { $bitmap.Save($path,[Drawing.Imaging.ImageFormat]::Png) } finally { $bitmap.Dispose() }
}
$arguments = "-w new new-tab -d `"$fresh`" -- cmd.exe /d /c `"echo public-mullion-shell>session-proof.txt & ping -n 90 127.0.0.1 >nul`""
$process = Start-Process "$fresh/mullion.exe" -ArgumentList $arguments -WorkingDirectory $fresh -PassThru
$output = Join-Path $env:RUNNER_TEMP 'mullion-visual-proof'
New-Item -ItemType Directory $output | Out-Null
try {
 $deadline = (Get-Date).AddSeconds(45)
 do {Start-Sleep -Milliseconds 500;$process.Refresh()} while (-not $process.HasExited -and (-not $process.MainWindowHandle -or -not (Test-Path "$fresh/session-proof.txt")) -and (Get-Date) -lt $deadline)
 if ($process.HasExited -or -not $process.MainWindowHandle -or (Get-Content "$fresh/session-proof.txt" -Raw).Trim() -ne 'public-mullion-shell') { throw 'Actual public portable shell/window failed' }
 $handle=$process.MainWindowHandle
 Capture-Owned $handle (Join-Path $output 'terminal.png')
 if (-not [OwnedWindow]::SetForegroundWindow($handle)) { throw 'Cannot focus exact owned window' }
 [Windows.Forms.SendKeys]::SendWait('^+{F12}')
 Start-Sleep -Seconds 2
 $window=[Windows.Automation.AutomationElement]::FromHandle($handle)
 $condition=[Windows.Automation.PropertyCondition]::new([Windows.Automation.AutomationElement]::AutomationIdProperty,'AboutTabs')
 $about=$window.FindFirst([Windows.Automation.TreeScope]::Descendants,$condition)
 if ($null -eq $about) { throw 'Actual About tabs are not visible' }
 $names=@($about.FindAll([Windows.Automation.TreeScope]::Descendants,[Windows.Automation.Condition]::TrueCondition) | ForEach-Object {$_.Current.Name})
 if (-not @($names | Where-Object {$_ -match '^Mullion'}).Count) { throw 'Actual About display name is not Mullion' }
 if (-not @($names | Where-Object {$_ -match '8fc3229'}).Count) { throw 'Actual About commit does not match public archive' }
 Capture-Owned $handle (Join-Path $output 'about.png')
 [Windows.Forms.SendKeys]::SendWait('{ESC}')
 [Windows.Forms.SendKeys]::SendWait('^,')
 Start-Sleep -Seconds 2
 $names=@($window.FindAll([Windows.Automation.TreeScope]::Descendants,[Windows.Automation.Condition]::TrueCondition) | ForEach-Object {$_.Current.Name})
 if (-not @($names | Where-Object {$_ -eq 'Session Restore'}).Count) { throw 'Actual settings Session Restore navigation is not visible' }
 Capture-Owned $handle (Join-Path $output 'settings.png')
 @{publicArchiveSHA='730300488a32d2ef1479b0e1a6687a2a3f5c14bfe3df1d9ad86c4f67b3d27b22';source='8fc3229d6558164b5bd9f0fcfbbcaa31c22035cc';actualShell=$true;aboutMullionDisplayName=$true;aboutCommit=$true;settingsSessionRestoreNavigation=$true;scope='Actual public portable ZIP on dedicated Windows runner; screenshots need visual review, navigation is not session restoration proof'} | ConvertTo-Json | Set-Content "$output/receipt.json"
} finally {
 if (-not $process.HasExited) { $null=$process.CloseMainWindow();if(-not $process.WaitForExit(5000)){Stop-Process -Id $process.Id} }
}
