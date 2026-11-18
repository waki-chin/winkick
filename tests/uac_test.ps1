# WinKick - live UAC elevation hand-off test (user must APPROVE the UAC prompt).
# Launches unelevated GUI, clicks Apply Selected (Dry Run OFF), watches for the elevated instance,
# and verifies the %TEMP% job file was created and deleted. Uses managed EnumWindows like gui_test.ps1.
# SAFETY: this script clicks Apply with elevation enabled and fires a REAL UAC prompt. It refuses to
# run unless the operator types YES at the confirmation prompt below (or passes -Yes on the command
# line). Running it as part of an automated batch is NOT supported.
param([switch]$Yes)
$ErrorActionPreference = 'Stop'

if (-not $Yes) {
    Write-Host '== WinKick live UAC test ==' -ForegroundColor Yellow
    Write-Host 'This will click Apply Selected with Dry Run OFF and fire a REAL UAC prompt.' -ForegroundColor Yellow
    Write-Host 'The default selection runs only harmless read-only commands (ver, two echoes).' -ForegroundColor Yellow
    $answer = Read-Host 'Type YES (all caps) to continue, anything else to abort'
    if ($answer -cne 'YES') { Write-Host 'Aborted - no UAC prompt was shown.'; exit 0 }
}

$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\winkick.exe'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this test unelevated: it must verify the UAC boundary, not an already-admin process.'
}
$workDir = Join-Path $PSScriptRoot ('out\uac-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $workDir | Out-Null
$script:logPath = Join-Path $workDir 'uac.log'
$iniPath = Join-Path $workDir 'safe.ini'
[IO.File]::WriteAllText($iniPath, "[Test | UAC Echo]`nparam=check:1`ncmd=echo uac-safe-success`n", (New-Object Text.UTF8Encoding($false)))
function Read-TestLog {
    if (-not (Test-Path $script:logPath)) { return '' }
    $stream = [IO.File]::Open($script:logPath, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class Uu {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr lParam);
    public delegate bool EnumChildProc(IntPtr h, IntPtr lParam);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder buf, int n);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc proc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr p, EnumChildProc proc, IntPtr lParam);
    [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    public const uint BM_CLICK = 0x00F5;
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
    public static Dictionary<int, IntPtr> GetChildren(IntPtr parent) {
        var map = new Dictionary<int, IntPtr>();
        EnumChildWindows(parent, (h, l) => { map[GetDlgCtrlID(h)] = h; return true; }, IntPtr.Zero);
        return map;
    }
}
'@ | Out-Null

Write-Host "Launching a dedicated safe UAC fixture (reboot and Explorer restart disabled)..." -ForegroundColor Cyan
$beforePids = @(Get-Process winkick -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
$beforeJobs = @(Get-ChildItem "$env:TEMP\WKJ*.tmp" -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$proc = Start-Process $exe -ArgumentList "--offline --config `"$iniPath`" --log=`"$script:logPath`" --no-reboot --no-explorer-restart" -WorkingDirectory $workDir -PassThru
Start-Sleep -Seconds 2
$hwnd = [Uu]::FindMainForPid([uint32]$proc.Id)
if ($hwnd -eq [IntPtr]::Zero) { Write-Host "FAIL: main window not found" -ForegroundColor Red; exit 1 }
Write-Host "Window found: $hwnd  (pid $($proc.Id))"

$children = [Uu]::GetChildren($hwnd)
Write-Host "Control IDs found: $(($children.Keys | Sort-Object) -join ', ')"
$applyBtn = $children[1007]
if (-not $applyBtn) { Write-Host "FAIL: Apply button (1007) not found" -ForegroundColor Red; exit 1 }

Write-Host "Clicking Apply Selected (Dry Run is OFF)..." -ForegroundColor Yellow
Write-Host ">>> APPROVE THE UAC PROMPT NOW <<<" -ForegroundColor Yellow
[void][Uu]::SendMessage($applyBtn, [Uu]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero)

# Wait for this fixture's log, not merely any process called winkick.
$elevated = $null
$deadline = (Get-Date).AddSeconds(60)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 200
    $elevated = Get-Process winkick -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $proc.Id -and $_.Id -notin $beforePids } | Select-Object -First 1
    if (Test-Path $script:logPath) {
        $log = Read-TestLog
        if ($log -match 'Finished: 1 succeeded, 0 failed') { break }
    }
}
$log = Read-TestLog
$newJobs = @(Get-ChildItem "$env:TEMP\WKJ*.tmp" -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notin $beforeJobs })
$ok = $proc.HasExited -and ($log -match 'Admin: Yes') -and
    ($log -match 'WinKick job file \(one-time, deleted after loading\)') -and
    ($log -match 'OUT: uac-safe-success') -and ($log -match 'Finished: 1 succeeded, 0 failed') -and
    ($newJobs.Count -eq 0)
if ($ok) { Write-Host 'PASS: unelevated launcher exited; elevated snapshot executed; job file consumed; zero failures.' -ForegroundColor Green }
else { Write-Host "FAIL: UAC round-trip assertions failed. Inspect $script:logPath" -ForegroundColor Red }

# Close only the elevated process running this unique test fixture. A High-IL helper is
# required because UIPI blocks unelevated WM_CLOSE/termination. Verify the log path in
# its command line before terminating, so a concurrent user instance is never touched.
if ($elevated) {
    $cleanupPath = Join-Path $workDir 'close-test.ps1'
    $escapedLog = $script:logPath.Replace("'", "''")
    $cleanup = @"
`$p = Get-CimInstance Win32_Process -Filter 'ProcessId = $($elevated.Id)'
if (`$p -and `$p.Name -eq 'winkick.exe' -and `$p.CommandLine.Contains('$escapedLog')) {
    Stop-Process -Id `$p.ProcessId -Force
}
"@
    [IO.File]::WriteAllText($cleanupPath, $cleanup, (New-Object Text.UTF8Encoding($false)))
    Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$cleanupPath`"" -Wait
}
if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
Write-Host "UAC evidence retained at $workDir"
if ($ok) { exit 0 } else { exit 1 }
