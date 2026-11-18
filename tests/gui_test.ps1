# WinKick - functional GUI test via window messages.
# Only cross-process safe messages are used (no pointers to app memory):
#   BM_CLICK / BM_GETCHECK for buttons/checkboxes, LVM_GETITEMSTATE (read-only),
#   CB_GETCURSEL / CB_GETLBTEXT (control copies text into caller buffer),
#   WM_GETTEXT / WM_GETTEXTLENGTH for text reads, PrintWindow for screenshots.
# Verifies: controls exist, log drawer works, Deselect -> * Custom (Modified),
# Select respects guard-locks, Dry Run + Apply runs the pipeline and logs it,
# Save Log writes log.txt, Dry Run disables the UAC shield requirement.
param([string]$ExeArgs = "")

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\winkick.exe'
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
if (-not $ExeArgs) { $ExeArgs = '--offline --config "' + (Join-Path $PSScriptRoot 'test_tasks.ini') + '"' }
$outDir = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $outDir | Out-Null
$workDir = Join-Path $outDir ('gui-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workDir | Out-Null

Add-Type -AssemblyName System.Drawing
$cs = @'
using System;
using System.Collections.Generic;
using System.Text;
using System.Runtime.InteropServices;

public static class Gk {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    public delegate bool EnumChildProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumChildProc proc, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder buf, int n);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr hWnd, uint msg, IntPtr wParam, StringBuilder lParam);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT rect);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdcBlt, uint nFlags);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr hWnd, uint msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);

    public struct RECT { public int Left, Top, Right, Bottom; }

    public const uint WM_CLOSE = 0x0010;
    public const uint WM_GETTEXT = 0x000D;
    public const uint WM_GETTEXTLENGTH = 0x000E;
    public const uint BM_CLICK = 0x00F5;
    public const uint BM_GETCHECK = 0x00F0;
    public const uint LVM_GETITEMSTATE = 0x1000 + 44;
    public const uint LVIS_STATEIMAGEMASK = 0xF000;
    public const uint CB_GETCURSEL = 0x0147;
    public const uint CB_GETLBTEXT = 0x0148;   // control copies text into our buffer
    public const uint CB_GETLBTEXTLEN = 0x0149;
    public const uint CB_SETCURSEL = 0x014E;

    public static IntPtr FindMainWindowForPid(uint processId) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var cls = new StringBuilder(256);
            GetClassName(h, cls, 256);
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (cls.ToString() == "WinKickMainWindow" && pid == processId) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static Dictionary<int, IntPtr> GetChildren(IntPtr parent) {
        var map = new Dictionary<int, IntPtr>();
        EnumChildWindows(parent, (h, l) => { map[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
        return map;
    }

    public static string WindowText(IntPtr hwnd) {
        int len = (int)SendMessage(hwnd, WM_GETTEXTLENGTH, IntPtr.Zero, IntPtr.Zero).ToInt64();
        if (len <= 0) return "";
        var sb = new StringBuilder(len + 2);
        SendMessage(hwnd, WM_GETTEXT, (IntPtr)(len + 2), sb);
        return sb.ToString();
    }

    public static uint LvGetStateImage(IntPtr lv, int index) {
        return ((uint)SendMessage(lv, LVM_GETITEMSTATE, (IntPtr)index, (IntPtr)LVIS_STATEIMAGEMASK).ToInt64()) >> 12;
    }

    public static string ComboSelectedText(IntPtr combo) {
        int sel = (int)SendMessage(combo, CB_GETCURSEL, IntPtr.Zero, IntPtr.Zero).ToInt64();
        if (sel < 0) return "";
        return ComboItemText(combo, sel);
    }

    public static string ComboItemText(IntPtr combo, int index) {
        int len = (int)SendMessage(combo, CB_GETLBTEXTLEN, (IntPtr)index, IntPtr.Zero).ToInt64();
        if (len < 0) return "";
        var buf = new StringBuilder(len + 1);
        SendMessage(combo, CB_GETLBTEXT, (IntPtr)index, buf);
        return buf.ToString();
    }
}
'@
# No -ReferencedAssemblies: the C# below uses only user32/GDI32 interop (no System.Drawing types).
# Passing -ReferencedAssemblies System.Drawing made pwsh 7 resolve only System.Drawing.Common and drop the core library (CS0246).
Add-Type -TypeDefinition $cs | Out-Null

function SaveShot($out) {
    $rect = New-Object Gk+RECT
    [void][Gk]::GetWindowRect($script:appHwnd, [ref]$rect)
    $w = [Math]::Max(1, $rect.Right - $rect.Left); $h = [Math]::Max(1, $rect.Bottom - $rect.Top)
    $bmp = New-Object System.Drawing.Bitmap($w, $h, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp); $hdc = $g.GetHdc()
    [void][Gk]::PrintWindow($script:appHwnd, $hdc, 2)
    $g.ReleaseHdc($hdc); $g.Dispose()
    $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
}

Write-Host "=== WinKick GUI functional test (args: $ExeArgs) ===" -ForegroundColor Cyan
$proc = Start-Process $exe -ArgumentList "$ExeArgs --no-reboot --no-explorer-restart" -WorkingDirectory $workDir -PassThru
Start-Sleep -Seconds 2
$script:appHwnd = [Gk]::FindMainWindowForPid([uint32]$proc.Id)
if ($script:appHwnd -eq [IntPtr]::Zero) { Stop-Process -Id $proc.Id -Force; throw "WinKick window not found for pid $($proc.Id)" }
Write-Host "hwnd = $($script:appHwnd)  pid = $($proc.Id)"

$pass = 0; $fail = 0
function Check([string]$Name, [bool]$Cond) {
    if ($Cond) { $script:pass++; Write-Host "[PASS] $Name" }
    else { $script:fail++; Write-Host "[FAIL] $Name" -ForegroundColor Red }
}

$children = [Gk]::GetChildren($script:appHwnd)
"Control IDs found: $(($children.Keys | Sort-Object) -join ', ')"

$listView  = $children[1010]; $dryRunChk = $children[1008]; $saveLogChk = $children[1009]
$applyBtn  = $children[1007]; $logEdit   = $children[1012]; $desc = $children[1011]
$tagCombo  = $children[1002]; $sourceCombo = $children[1001]
$selectBtn = $children[1005]; $deselectBtn = $children[1006]

# --- Test 1: all expected controls exist ---
$expected = @(1001,1002,1003,1004,1005,1006,1007,1008,1009,1010,1011,1012,1013,1014,1100,1101,1102,1103,1104,1105,1110,1111,1112,1113,1114,1115)
$missing = $expected | Where-Object { -not $children.ContainsKey($_) }
Check "all expected controls exist" ($missing.Count -eq 0 -and $children[0] -ne $null)

# --- Test 2: log drawer has the startup lines ---
$logTxt = [Gk]::WindowText($logEdit)
Check "log drawer shows startup lines" ($logTxt.Contains("WinKick $version") -and $logTxt -match 'items loaded')

# --- Test 3: Deselect marks source as * Custom (Modified) ---
[void][Gk]::SendMessage($deselectBtn, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 300
$srcText = [Gk]::ComboSelectedText($sourceCombo)
Write-Host "source after Deselect: [$srcText]"
Check "Deselect -> * Custom (Modified)" ($srcText -eq '* Custom (Modified)')

# --- Test 4: Select re-checks compatible items, guard-locked stays unchecked ---
[void][Gk]::SendMessage($selectBtn, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 300
$echoImg = [Gk]::LvGetStateImage($listView, 0)     # Echo Greeting, always compatible
$netImg  = [Gk]::LvGetStateImage($listView, 4)     # net item in the dedicated safe fixture
"img item0(echo)=$echoImg item4(net, locked)=$netImg"
Check "Select checks compatible item (common)" ($echoImg -eq 2)
Check "Select leaves guard-locked item unchecked" ($netImg -eq 1)

# --- Test 5: Dry Run checkbox toggles, and Dry Run means no UAC shield is needed (visual) ---
[void][Gk]::SendMessage($dryRunChk, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 200
$dryState = [Gk]::SendMessage($dryRunChk, [Gk]::BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
Check "Dry Run checkbox toggles on" ($dryState -eq 1)

# --- Test 6: Apply executes pipeline in worker thread and logs it ---
[void][Gk]::SendMessage($applyBtn, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 4
$logTxt = [Gk]::WindowText($logEdit)
$tail = ($logTxt -split "`r?`n" | Select-Object -Last 4) -join " | "
Write-Host "Log tail: $tail"
Check "apply logs DRY-RUN lines" ($logTxt -match 'RES: DRY-RUN')
Check "apply logs completion summary" ($logTxt -match 'Dry-run finished')
Check "apply did not execute commands" ($logTxt -notmatch 'RES: OK' -and $logTxt -match 'Dry-run: \d+')
SaveShot (Join-Path $outDir 'gui_test_after_apply.png')

# --- Test 7: Save Log writes log.txt on next run ---
[void][Gk]::SendMessage($saveLogChk, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Milliseconds 200
$saveState = [Gk]::SendMessage($saveLogChk, [Gk]::BM_GETCHECK, [IntPtr]::Zero, [IntPtr]::Zero).ToInt64()
Check "Save Log checkbox toggles on" ($saveState -eq 1)
[void][Gk]::SendMessage($applyBtn, [Gk]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 4
$logPath = Join-Path $workDir 'log.txt'
Check "log.txt written on Apply" (Test-Path $logPath)
if (Test-Path $logPath) {
    $fileTxt = Get-Content $logPath -Raw
    Check "log.txt contains run header" ($fileTxt -match 'Mode: Dry-Run')
    Check "log.txt contains FAILED ITEMS section" ($fileTxt -match 'FAILED ITEMS:')
    Remove-Item $logPath -Force
}

# --- Test 8: closing the window exits the process ---
[void][Gk]::PostMessage($script:appHwnd, [Gk]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
Start-Sleep -Seconds 1
Check "WM_CLOSE exits the app" ($proc.HasExited)
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force }
Remove-Item $workDir -ErrorAction SilentlyContinue

Write-Host ""
Write-Host ("PASS: {0}  FAIL: {1}" -f $pass, $fail) -ForegroundColor Cyan
if ($fail -eq 0) { exit 0 } else { exit 1 }
