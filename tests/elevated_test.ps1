# Safe, real elevated CLI/GUI checks. Never reboots or restarts Explorer.
param([string]$WorkDir)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\winkick.exe'
if (-not $WorkDir) {
    $WorkDir = Join-Path $PSScriptRoot ('out\elevated-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $WorkDir | Out-Null
}
$resultPath = Join-Path $WorkDir 'results.json'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $child = Start-Process powershell.exe -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -WorkDir `"$WorkDir`"" -Wait -PassThru
    if (Test-Path $resultPath) {
        $result = Get-Content $resultPath -Raw | ConvertFrom-Json
        $result.Checks | ForEach-Object { Write-Output $_ }
        Write-Output "Evidence: $WorkDir"
    }
    exit $child.ExitCode
}

$checks = New-Object 'Collections.Generic.List[string]'
$script:fail = 0
function Check([string]$name, [bool]$ok) {
    if ($ok) { $checks.Add("[PASS] $name") }
    else { $checks.Add("[FAIL] $name"); $script:fail++ }
}
function Run-WinKick([string[]]$ExeArgs) {
    $p = New-Object Diagnostics.Process
    $p.StartInfo.FileName = $exe
    $p.StartInfo.WorkingDirectory = $WorkDir
    $p.StartInfo.Arguments = ((@($ExeArgs) + @('--no-reboot','--no-explorer-restart')) | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardOutput = $true
    $p.StartInfo.RedirectStandardError = $true
    [void]$p.Start()
    $out = $p.StandardOutput.ReadToEndAsync()
    $err = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); throw 'Elevated CLI test timed out.' }
    $r = [pscustomobject]@{ Out=$out.GetAwaiter().GetResult(); Err=$err.GetAwaiter().GetResult(); Exit=$p.ExitCode }
    $p.Dispose()
    return $r
}

$gui = $null
try {
    $ini = Join-Path $WorkDir 'safe.ini'
    [IO.File]::WriteAllText($ini, "[Test | Success]`nparam=check:1`ntags=common,reboot,restart-explorer`ncmd=echo elevated-safe-success`n[Test | Failure]`ntags=failure`ncmd=exit 7`n")
    $r = Run-WinKick @('--silent','--offline','--config',$ini)
    Check 'elevated CLI success exits 0' ($r.Exit -eq 0 -and $r.Out -match 'Admin: Yes' -and $r.Out -match 'OUT: elevated-safe-success')
    Check 'lifecycle suppression honored on real tasks' ($r.Out -notmatch 'Restarting explorer|Reboot initiated|Reboot required')
    $r = Run-WinKick @('--silent','--offline','--config',$ini,'--tags','failure')
    Check 'real task failure exits 1 and records exit 7' ($r.Exit -eq 1 -and $r.Out -match 'exit=7' -and $r.Out -match 'Failed: 1')
    $log = Join-Path $WorkDir 'quiet.log'
    $r = Run-WinKick @('--silent','--offline','--config',$ini,'--quiet',"--log=$log",'--reboot')
    Check 'quiet mode suppresses stdout' ($r.Exit -eq 0 -and $r.Out.Length -eq 0)
    Check 'quiet mode still writes successful UTF-8 log' ((Test-Path $log) -and ([IO.File]::ReadAllText($log) -match 'Succeeded: 1 \| Failed: 0'))
    $r = Run-WinKick @('--silent','--offline','--config',$ini,'--quiet',"--log=$(Join-Path $WorkDir 'missing\log.txt')")
    Check 'unwritable log warns on stderr even with quiet' ($r.Exit -eq 0 -and $r.Err -match 'WARNING: could not open log file')

    Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class AuditUi {
    public delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder b, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, StringBuilder l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    public static IntPtr FindMain(uint target) {
        IntPtr result = IntPtr.Zero;
        EnumWindows((h,p) => {
            uint pid; GetWindowThreadProcessId(h, out pid);
            var c = new StringBuilder(128); GetClassName(h,c,c.Capacity);
            if (pid == target && c.ToString() == "WinKickMainWindow") { result=h; return false; }
            return true;
        }, IntPtr.Zero);
        return result;
    }
    public static string Text(IntPtr h) {
        int len = (int)SendMessage(h,14,IntPtr.Zero,IntPtr.Zero);
        var b = new StringBuilder(len+1); SendMessage(h,13,(IntPtr)b.Capacity,b);
        return b.ToString();
    }
}
'@
    $streamIni = Join-Path $WorkDir 'stream.ini'
    [IO.File]::WriteAllText($streamIni, "[Test | Streaming]`nparam=check:1`ncmd=echo stream-before & ping -n 4 127.0.0.1 >nul & echo stream-after`n")
    $streamLog = Join-Path $WorkDir 'stream.log'
    $gui = Start-Process $exe -ArgumentList "--offline --config `"$streamIni`" --log=`"$streamLog`" --auto-run --no-reboot --no-explorer-restart" -WorkingDirectory $WorkDir -PassThru
    $deadline = (Get-Date).AddSeconds(15)
    $h = [IntPtr]::Zero
    $live = $false
    while ((Get-Date) -lt $deadline -and -not $gui.HasExited) {
        $h = [AuditUi]::FindMain([uint32]$gui.Id)
        if ($h -ne [IntPtr]::Zero) {
            $text = [AuditUi]::Text([AuditUi]::GetDlgItem($h,1012))
            if ($text -match 'OUT: stream-before' -and $text -notmatch 'OUT: stream-after|RES: OK') { $live=$true; break }
        }
        Start-Sleep -Milliseconds 50
    }
    Check 'GUI receives child output before command completion' $live
    if ($h -ne [IntPtr]::Zero) { [void][AuditUi]::PostMessage($h,16,[IntPtr]::Zero,[IntPtr]::Zero) }
    Check 'close during a real run exits without completion modal' ($gui.WaitForExit(15000))
    Check 'close waits for command to complete and log to flush' ((Test-Path $streamLog) -and
        ([IO.File]::ReadAllText($streamLog) -match 'Succeeded: 1 \| Failed: 0'))
    if (-not $gui.HasExited) { $gui.Kill() }

    # More status posts than the Windows message-queue quota; no commands execute.
    $largeIni = Join-Path $WorkDir 'large.ini'
    $builder = New-Object Text.StringBuilder
    for ($i=0; $i -lt 12000; $i++) { [void]$builder.Append("[Stress | Task $i]`nparam=check:1`ncmd=echo safe`n") }
    [IO.File]::WriteAllText($largeIni,$builder.ToString())
    $gui = Start-Process $exe -ArgumentList "--offline --config `"$largeIni`" --no-reboot --no-explorer-restart" -WorkingDirectory $WorkDir -PassThru
    $deadline = (Get-Date).AddSeconds(60)
    $h = [IntPtr]::Zero
    while ((Get-Date) -lt $deadline -and $h -eq [IntPtr]::Zero -and -not $gui.HasExited) {
        $h=[AuditUi]::FindMain([uint32]$gui.Id)
        Start-Sleep -Milliseconds 100
    }
    if ($h -eq [IntPtr]::Zero) { throw 'Large GUI fixture failed to create a window.' }
    if (-not $gui.WaitForInputIdle(60000)) { throw 'Large GUI fixture did not finish initialization.' }
    # WaitForInputIdle alone is insufficient to detect run completion; assert the footer.
    [void][AuditUi]::SendMessage([AuditUi]::GetDlgItem($h,1008),245,[IntPtr]::Zero,[IntPtr]::Zero)
    [void][AuditUi]::SendMessage([AuditUi]::GetDlgItem($h,1007),245,[IntPtr]::Zero,[IntPtr]::Zero)
    $deadline = (Get-Date).AddSeconds(60)
    $text = ''
    while ((Get-Date) -lt $deadline -and -not $gui.HasExited) {
        $text=[AuditUi]::Text([AuditUi]::GetDlgItem($h,1012))
        if ($text -match 'Dry-run finished: 12000 simulated') { break }
        Start-Sleep -Milliseconds 100
    }
    Check '12000-task dry-run finishes without lost completion message' ($text -match 'Dry-run finished: 12000 simulated')
    Check 'bounded GUI log retains final summary' ($text.Length -le 400000 -and $text -match 'FAILED ITEMS:')
    [void][AuditUi]::PostMessage($h,16,[IntPtr]::Zero,[IntPtr]::Zero)
    Check 'large dry-run window closes cleanly' ($gui.WaitForExit(10000))
} catch {
    Check ("unexpected exception: " + $_.Exception.Message) $false
} finally {
    if ($gui -and -not $gui.HasExited) { $gui.Kill() }
    [pscustomobject]@{ Checks=@($checks.ToArray()); Failed=$script:fail } |
        ConvertTo-Json -Depth 4 | Set-Content $resultPath -Encoding UTF8
}
if ($script:fail -eq 0) { exit 0 } else { exit 1 }
