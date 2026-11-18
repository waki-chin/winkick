# WinKick - live interactive test: input dialog ("Load from URL...") Enter/Tab handling AND the
# main window Tab/Shift+Tab traversal + post-run focus restore (requires an interactive desktop).
# Verifies:
#   A) VK_RETURN with focus on the Cancel button cancels the dialog (does NOT fetch).
#   B) VK_RETURN with initial focus in the edit box accepts (empty URL -> fetch error box proves IDOK path ran).
#   C) Main window: focus starts on the ListView; 30x Shift+Tab and 15x Tab stay responsive
#      (pre-round-16 code spun forever here); dry-run Apply with Shift+Tab spam completes;
#      focus is restored to the Apply button afterwards.
#   D) Input dialog focus starts in the edit box (counted in the checks below).
# Uses cross-process messages (BM_CLICK, SendInput for real key events), GetGUIThreadInfo-adjacent
# measurement via AttachThreadInput+GetFocus (reads the app thread's input state directly).
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\winkick.exe'

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Dl {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder buf, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint idThread, IntPtr pgui);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    public const uint WM_COMMAND   = 0x0111;
    public const uint WM_KEYDOWN   = 0x0100;
    public const uint WM_KEYUP     = 0x0101;
    public const uint WM_CLOSE     = 0x0010;
    public const uint VK_RETURN    = 0x0D;
    public const uint VK_TAB       = 0x09;
    public const uint BM_CLICK     = 0x00F5;
    [StructLayout(LayoutKind.Sequential)] public struct GUITHREADINFO {
        public int cbSize; public uint flags;
        public IntPtr hwndActive, hwndFocus, hwndCapture, hwndCaret;
        public RECT rcCaret;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [DllImport("user32.dll")] public static extern bool GetGUIThreadInfo(uint idThread, ref GUITHREADINFO pgui);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetActiveWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool IsChild(IntPtr hWndParent, IntPtr hWndChild);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern uint WaitForInputIdle(IntPtr hProcess, uint dwMilliseconds);
    [DllImport("user32.dll")] public static extern uint SendInput(uint nInputs, INPUT[] pInputs, int cbSize);
    public const uint INPUT_KEYBOARD = 1;
    public const uint KEYEVENTF_KEYUP = 0x0002;
    public const uint VK_SHIFT = 0x10;
    public const uint WAIT_OBJECT_0 = 0;
    public const uint WAIT_TIMEOUT = 0x102;
    [StructLayout(LayoutKind.Sequential)] public struct MOUSEINPUT { public int dx; public int dy; public uint mouseData; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT { public ushort wVk; public ushort wScan; public uint dwFlags; public uint time; public IntPtr dwExtraInfo; }
    [StructLayout(LayoutKind.Sequential)] public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL; public ushort wParamH; }
    [StructLayout(LayoutKind.Explicit)] public struct INPUTUNION {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public INPUTUNION u; }
    public static uint SendInput2(INPUT[] inputs) { return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf(typeof(INPUT))); }
    public static uint TypeKey(ushort vk, bool keyUp) {
        var i = new INPUT { type = INPUT_KEYBOARD, u = new INPUTUNION { ki = new KEYBDINPUT { wVk = vk, dwFlags = keyUp ? KEYEVENTF_KEYUP : 0 } } };
        return SendInput(1, new INPUT[] { i }, Marshal.SizeOf(typeof(INPUT)));
    }
    public static bool VK(ushort vk) { return TypeKey(vk, false) == 1 && TypeKey(vk, true) == 1; }
    public static bool ShiftTab() {
        if (TypeKey((ushort)VK_SHIFT, false) != 1) return false;
        bool ok = TypeKey((ushort)VK_TAB, false) == 1 && TypeKey((ushort)VK_TAB, true) == 1;
        TypeKey((ushort)VK_SHIFT, true);
        return ok;
    }
    public static bool Tab() { return VK((ushort)VK_TAB); }
    public static bool Return() { return VK(0x0D); }
    public delegate bool EnumChildProc(IntPtr h, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumChildProc proc, IntPtr lParam);
    public static Dictionary<int, IntPtr> GetChildren(IntPtr parent) {
        var map = new Dictionary<int, IntPtr>();
        EnumChildWindows(parent, (h, l) => { map[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
        return map;
    }

    // Force the target window to own keyboard focus from this (same-integrity) process.
    public static bool ForceActivate(IntPtr hWnd) {
        IntPtr fg = GetForegroundWindow();
        uint fgTid = ThreadOf(fg);
        uint appTid = ThreadOf(hWnd);
        uint ourTid = GetCurrentThreadId();
        bool attached = false;
        if (ourTid != fgTid) attached = AttachThreadInput(ourTid, fgTid, true);
        bool ok = SetForegroundWindow(hWnd);
        if (attached) AttachThreadInput(ourTid, fgTid, false);
        if (!ok) { attached = AttachThreadInput(ourTid, appTid, true); ok = SetForegroundWindow(hWnd); if (attached) AttachThreadInput(ourTid, appTid, false); }
        return ok;
    }

    public static IntPtr FindMainForPid(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var cls = new StringBuilder(64);
            GetClassName(h, cls, 64);
            uint p; GetWindowThreadProcessId(h, out p);
            if (cls.ToString() == "WinKickMainWindow" && p == pid) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindDialogForPid(uint pid, string cls) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var c = new StringBuilder(64);
            GetClassName(h, c, 64);
            uint p; GetWindowThreadProcessId(h, out p);
            if (c.ToString() == cls && p == pid && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FindMessageBoxForPid(uint pid) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            var c = new StringBuilder(64);
            GetClassName(h, c, 64);
            uint p; GetWindowThreadProcessId(h, out p);
            if (c.ToString() == "#32770" && p == pid && IsWindowVisible(h)) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
    public static uint ThreadOf(IntPtr h) { uint p; return GetWindowThreadProcessId(h, out p); }
    public static IntPtr FocusOf(uint tid) {
        var g = new GUITHREADINFO();
        g.cbSize = Marshal.SizeOf(g);
        if (!GetGUIThreadInfo(tid, ref g)) return IntPtr.Zero;
        return g.hwndFocus;
    }
    [DllImport("user32.dll")] public static extern IntPtr GetFocus();
    [DllImport("user32.dll")] public static extern IntPtr GetActiveWindow();
    // Cross-process focus measurement: attach our input queue to the target thread's so that
    // GetFocus()/GetActiveWindow() see the same input state. Same-integrity requirement applies.
    public static IntPtr FocusAttached(IntPtr targetWindow) {
        uint appTid = ThreadOf(targetWindow);
        uint ourTid = GetCurrentThreadId();
        bool attached = false;
        if (ourTid != appTid) attached = AttachThreadInput(ourTid, appTid, true);
        IntPtr f = GetFocus();
        if (attached) AttachThreadInput(ourTid, appTid, false);
        return f;
    }
    public static void PostKey(IntPtr h, uint vk) {
        PostMessage(h, WM_KEYDOWN, (IntPtr)vk, IntPtr.Zero);
        PostMessage(h, WM_KEYUP, (IntPtr)vk, IntPtr.Zero);
    }
}
'@ -ReferencedAssemblies @() | Out-Null

function Start-WingetKick() {
    $proc = Start-Process $exe -ArgumentList '--offline --no-reboot --no-explorer-restart' -WorkingDirectory $root -PassThru
    Start-Sleep -Seconds 2
    $hwnd = [Dl]::FindMainForPid([uint32]$proc.Id)
    if ($hwnd -eq [IntPtr]::Zero) { Write-Host "FAIL: main window not found" -ForegroundColor Red; exit 1 }
    return @{ proc = $proc; hwnd = $hwnd }
}
function Close-Gracefully($s) {
    [void][Dl]::PostMessage($s.hwnd, [Dl]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 600
    try { Stop-Process -Id $s.proc.Id -Force -ErrorAction Stop } catch {}
}
function Wait-ForClickActivate($h, $seconds) {
    Write-Host '>>> PLEASE CLICK the WinKick window on your desktop now <<<' -ForegroundColor Yellow
    Write-Host ("Waiting up to {0} seconds; focus checks are skipped if you do not." -f $seconds) -ForegroundColor Yellow
    $end = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $end) {
        Start-Sleep -Milliseconds 500
        if ([Dl]::GetForegroundWindow() -eq $h) { Write-Host 'Window is foreground; proceeding with focus checks.' -ForegroundColor Green; return $true }
    }
    Write-Host 'No click detected; skipping focus-dependent checks.' -ForegroundColor Yellow
    return $false
}
function Open-UrlDialog($s) {
    # Same code path as clicking "+ Load..." -> "From URL...": WM_COMMAND with MENU_LOAD_FROM_URL
    [void][Dl]::PostMessage($s.hwnd, [Dl]::WM_COMMAND, [IntPtr]2002, [IntPtr]::Zero)
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 100
        $dlg = [Dl]::FindDialogForPid([uint32]$s.proc.Id, 'WinKickInputDialog')
        if ($dlg -ne [IntPtr]::Zero) { return $dlg }
    }
    return [IntPtr]::Zero
}
$script:fail = 0
$script:pass = 0
$script:skip = 0
function Check($label, $cond) {
    if ($cond) { $script:pass++; Write-Host "PASS: $label" -ForegroundColor Green } else { Write-Host "FAIL: $label" -ForegroundColor Red; $script:fail++ }
}

# ---------- Case A: Enter activates the FOCUSED control (round-16 regression oracle) ----------
# Real keys (SendInput) on a genuinely foreground dialog; focus measured via input-queue attach.
$s = Start-WingetKick
$dlg = Open-UrlDialog $s
Check 'A0: input dialog opened' ($dlg -ne [IntPtr]::Zero)
$focusWorks = $false
if ($dlg -ne [IntPtr]::Zero) {
    $focusWorks = [Dl]::ForceActivate($dlg)
    if (-not $focusWorks) { $focusWorks = Wait-ForClickActivate $dlg 30 }
    Start-Sleep -Milliseconds 300
    if ($focusWorks) {
        $f0 = [Dl]::FocusAttached($dlg)
        Write-Host ("diag: focus ctrlID (attached) = {0}" -f [Dl]::GetDlgCtrlID($f0))
        Check 'A1: initial focus is the edit box (1001)' (([Dl]::GetDlgCtrlID($f0)) -eq 1001)
        [void][Dl]::Tab(); Start-Sleep -Milliseconds 250
        $f1 = [Dl]::FocusAttached($dlg)
        Check 'A2: after first Tab focus is OK (1)' (([Dl]::GetDlgCtrlID($f1)) -eq 1)
        [void][Dl]::Tab(); Start-Sleep -Milliseconds 250
        $f2 = [Dl]::FocusAttached($dlg)
        $finalId = [int][Dl]::GetDlgCtrlID($f2)
        Write-Host ("diag: focus ctrlID after 2 Tabs = {0}" -f $finalId)
        Check 'A3: after second Tab focus is Cancel (2)' ($finalId -eq 2)
        [void][Dl]::Return(); Start-Sleep -Milliseconds 800
        Check 'A4: dialog closed after Enter' (-not [Dl]::IsWindow($dlg))
        Start-Sleep -Milliseconds 2500
        $box = [Dl]::FindMessageBoxForPid([uint32]$s.proc.Id)
        Check ("A5: Enter activated the focused control (id={0}, no error box expected for Cancel)" -f $finalId) ($box -eq [IntPtr]::Zero)
        if ($box -ne [IntPtr]::Zero) { [void][Dl]::PostMessage($box, [Dl]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero); Start-Sleep -Milliseconds 400 }
    } else {
        $script:skip += 5
        Write-Host 'NOTE: foreground not granted; keyboard-focus assertions skipped.' -ForegroundColor Yellow
    }
}
Close-Gracefully $s

# ---------- Case B: Enter in edit box accepts (IDOK path -> fetch error box) ----------
$s = Start-WingetKick
$dlg = Open-UrlDialog $s
Check 'B0: input dialog opened' ($dlg -ne [IntPtr]::Zero)
if ($dlg -ne [IntPtr]::Zero) {
    [void][Dl]::ForceActivate($dlg)
    Start-Sleep -Milliseconds 200
    [void][Dl]::PostKey($dlg, [Dl]::VK_RETURN); Start-Sleep -Milliseconds 800
    Check 'B1: dialog closed after Enter in edit' (-not [Dl]::IsWindow($dlg))
    $box = [IntPtr]::Zero
    for ($i = 0; $i -lt 50; $i++) {
        Start-Sleep -Milliseconds 100
        $box = [Dl]::FindMessageBoxForPid([uint32]$s.proc.Id)
        if ($box -ne [IntPtr]::Zero) { break }
    }
    # The prefilled "https://" fails to crack, proving the OK/accept path executed
    Check 'B2: OK path executed (fetch error box appeared)' ($box -ne [IntPtr]::Zero)
    # dismiss the message box
    [void][Dl]::PostMessage($box, [Dl]::WM_CLOSE, [IntPtr]::Zero, [IntPtr]::Zero)
    Start-Sleep -Milliseconds 400
}
Close-Gracefully $s

# ---------- Case C: main-window Tab/Shift+Tab (Live) ----------
$s = Start-WingetKick
$h = $s.hwnd
if ([Dl]::ForceActivate($h)) {
    $mainClick = $true
} else {
    $mainClick = Wait-ForClickActivate $h 30
}
if ($mainClick) {
    Start-Sleep -Milliseconds 300
    $f = [Dl]::FocusAttached($h)
    $focusId = [int][Dl]::GetDlgCtrlID($f)
    Write-Host ("diag: main-window focus ctrlID (attached) after raise = {0}" -f $focusId)
    Check 'C0: initial focus is the ListView (1010)' ($focusId -eq 1010)
    # C4/C5 first: with initial focus on the ListView (which gets disabled during the run), the
    # focus drops to NULL and must be restored to Apply afterwards. Doing the storms first could
    # park focus on logEdit (the one control that stays enabled), where restore is intentionally skipped.
    $children = [Dl]::GetChildren($h)
    $dryH = $children[1008]; $applyH = $children[1007]
    if ($dryH -and $applyH) {
        [void][Dl]::SendMessage($dryH, [Dl]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
        [void][Dl]::SendMessage($applyH, [Dl]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)
        $r = [Dl]::WaitForInputIdle($s.proc.Handle, 30000)
        Check 'C4: dry-run Apply completes (pump not frozen)' ($r -eq [Dl]::WAIT_OBJECT_0)
        Start-Sleep -Milliseconds 500
        $f = [Dl]::FocusAttached($h)
        $restoredId = [int][Dl]::GetDlgCtrlID($f)
        Write-Host ("diag: focus ctrlID after run = {0}" -f $restoredId)
        Check 'C5: focus restored to Apply button (1007) after run' ($restoredId -eq 1007)
    } else {
        $script:skip += 2
        Write-Host 'NOTE: could not enumerate child controls for C4/C5; skipped' -ForegroundColor Yellow
    }
    # C1: 30 Shift+Tabs with a responsiveness oracle (pre-fix code would spin forever here)
    for ($i = 0; $i -lt 30; $i++) { [void][Dl]::ShiftTab(); Start-Sleep -Milliseconds 30 }
    $r = [Dl]::WaitForInputIdle($s.proc.Handle, 3000)
    Check 'C1: app remains responsive after 30 Shift+Tab (no UI freeze)' ($r -eq [Dl]::WAIT_OBJECT_0)
    $f = [Dl]::FocusAttached($h)
    Check 'C2: focus is non-null and inside the window after Shift+Tab storm' (($f -ne [IntPtr]::Zero) -and [Dl]::IsChild($h, $f))
    # C3: 15 plain Tabs also stay responsive
    for ($i = 0; $i -lt 15; $i++) { [void][Dl]::Tab(); Start-Sleep -Milliseconds 30 }
    $r = [Dl]::WaitForInputIdle($s.proc.Handle, 3000)
    Check 'C3: app remains responsive after 15 Tab' ($r -eq [Dl]::WAIT_OBJECT_0)
} else {
    $script:skip += 6
    Write-Host 'NOTE: foreground activation not granted; main-window focus assertions skipped.' -ForegroundColor Yellow
}
Close-Gracefully $s

Write-Host ""
Write-Host "PASS: $script:pass  FAIL: $script:fail  SKIP: $script:skip"
if ($script:fail -eq 0) { Write-Host 'Executed dialog checks passed; skipped checks are not verified.' -ForegroundColor Green; exit 0 }
Write-Host "$script:fail DIALOG TEST(S) FAILED" -ForegroundColor Red; exit 1
