# UI automation helper for visual testing.
# Coordinates are in screenshot space (screenshot.ps1 scales the desktop to 1600 px width).
#   ui.ps1 click X Y        left click
#   ui.ps1 rclick X Y       right click
#   ui.ps1 dclick X Y       double click
#   ui.ps1 drag X1 Y1 X2 Y2 left-button drag
#   ui.ps1 wheel X Y N      mouse wheel (N notches, + = up)
#   ui.ps1 key KEYS         SendKeys syntax, e.g. "{F9}", "^o", "hello{ENTER}"
param([string]$Action, [double[]]$Args2 = @(), [string]$Keys = '')
Add-Type -AssemblyName System.Windows.Forms
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UiInput {
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x, int y);
    [DllImport("user32.dll")] public static extern void mouse_event(uint f, uint dx, uint dy, int data, UIntPtr extra);
    public const uint LDOWN = 0x02, LUP = 0x04, RDOWN = 0x08, RUP = 0x10, WHEEL = 0x800;
}
'@ -ErrorAction SilentlyContinue
[UiInput]::SetProcessDPIAware() | Out-Null
$b = [System.Windows.Forms.SystemInformation]::VirtualScreen
$scale = $b.Width / 1600.0
function P([double]$v) { [int]($v * $scale) }
function MoveTo([double]$x, [double]$y) { [UiInput]::SetCursorPos((P $x) + $b.Left, (P $y) + $b.Top) | Out-Null; Start-Sleep -Milliseconds 60 }
switch ($Action) {
    'click'  { MoveTo $Args2[0] $Args2[1]; [UiInput]::mouse_event([UiInput]::LDOWN, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40; [UiInput]::mouse_event([UiInput]::LUP, 0, 0, 0, [UIntPtr]::Zero) }
    'rclick' { MoveTo $Args2[0] $Args2[1]; [UiInput]::mouse_event([UiInput]::RDOWN, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 40; [UiInput]::mouse_event([UiInput]::RUP, 0, 0, 0, [UIntPtr]::Zero) }
    'dclick' { MoveTo $Args2[0] $Args2[1]; foreach ($i in 1..2) { [UiInput]::mouse_event([UiInput]::LDOWN, 0, 0, 0, [UIntPtr]::Zero); [UiInput]::mouse_event([UiInput]::LUP, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds 60 } }
    'drag'   {
        MoveTo $Args2[0] $Args2[1]; [UiInput]::mouse_event([UiInput]::LDOWN, 0, 0, 0, [UIntPtr]::Zero)
        $steps = 12
        for ($i = 1; $i -le $steps; $i++) { MoveTo ($Args2[0] + ($Args2[2] - $Args2[0]) * $i / $steps) ($Args2[1] + ($Args2[3] - $Args2[1]) * $i / $steps) }
        [UiInput]::mouse_event([UiInput]::LUP, 0, 0, 0, [UIntPtr]::Zero)
    }
    'wheel'  { MoveTo $Args2[0] $Args2[1]; [UiInput]::mouse_event([UiInput]::WHEEL, 0, 0, [int]($Args2[2] * 120), [UIntPtr]::Zero) }
    'key'    { [System.Windows.Forms.SendKeys]::SendWait($Keys) }
    default  { throw "unknown action $Action" }
}

