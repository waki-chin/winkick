# WinKick - GUI screenshot helper. Captures the WinKick window via PrintWindow
# (works even when occluded by other foreground windows).
# Usage: powershell -File screenshot.ps1 -Out out.png [-WaitMs 500]
param(
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$WaitMs = 500,
    [int]$OnlyPid = 0
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$cs = @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class Win32p {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder buf, int n);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    public const int GWL_STYLE = -16;
    public const int WS_VISIBLE = 0x10000000;
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
}
'@
Add-Type -TypeDefinition $cs

Start-Sleep -Milliseconds $WaitMs
$script:found = [IntPtr]::Zero
$cb = [Win32p+EnumWindowsProc] {
    param($h, $l)
    $class = New-Object System.Text.StringBuilder 256
    [void][Win32p]::GetClassName($h, $class, 256)
    $wpid = 0
    [void][Win32p]::GetWindowThreadProcessId($h, [ref]$wpid)
    if ($class.ToString() -eq "WinKickMainWindow" -and [Win32p]::IsWindowVisible($h) -and ($OnlyPid -eq 0 -or $wpid -eq $OnlyPid)) { $script:found = $h; return $false }
    return $true
}
[void][Win32p]::EnumWindows($cb, [IntPtr]::Zero)
$hwnd = $script:found
if ($hwnd -eq [IntPtr]::Zero) { throw "WinKick window not found" }
Write-Host "hwnd=$hwnd"

$rect = New-Object Win32p+RECT
[void][Win32p]::GetWindowRect($hwnd, [ref]$rect)
$w = $rect.Right - $rect.Left
$h = $rect.Bottom - $rect.Top
if ($w -le 0 -or $h -le 0) { throw "Invalid rect" }

$bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hdc = $g.GetHdc()
$ok = [Win32p]::PrintWindow($hwnd, $hdc, 2)   # 2 = PW_RENDERFULLCONTENT
$g.ReleaseHdc($hdc)
$g.Dispose()
if (-not $ok) { throw "PrintWindow failed" }
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bmp.Dispose()
Write-Host "Saved $Out (${w}x${h})"

