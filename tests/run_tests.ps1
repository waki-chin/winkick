# WinKick - safe test-suite runner
# Tests the CLI end-to-end plus a separate TEST-ONLY harness for the execution engine.
# Never reboots, never runs registry commands, never installs anything.
# Execution uses test_tasks.ini only, never the production setup catalog.
param([switch]$SkipBuild)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root 'build\winkick.exe'
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
if (-not (Test-Path $exe)) { throw 'Build the application first: .\build.bat' }
$testIni = Join-Path $PSScriptRoot 'test_tasks.ini'
$outDir  = Join-Path $PSScriptRoot 'out'
New-Item -ItemType Directory -Force $outDir | Out-Null

$script:pass = 0; $script:fail = 0
# win11 guards are host-relative (win11 = 10.0 build >= 22000); make the two
# win11 assertions host-aware so the suite is green on Windows 10 and Windows 11
$osBuild  = [Environment]::OSVersion.Version.Build
$isWin11  = ((([Environment]::OSVersion.Version.Major) -eq 10) -and ($osBuild -ge 22000))
Write-Host ("Host OS build: {0} (Windows 11 guard host: {1})" -f $osBuild, $(if ($isWin11) { 'yes' } else { 'no' }))
function Check([string]$Name, [bool]$Cond) {
    if ($Cond) { $script:pass++; Write-Host "[PASS] $Name" }
    else       { $script:fail++; Write-Host "[FAIL] $Name" -ForegroundColor Red }
}
function RunExe([string[]]$ExeArgs) {
    $p = New-Object Diagnostics.Process
    $p.StartInfo.FileName = $exe
    # CommandLineToArgvW quoting, including embedded quotes and trailing backslashes.
    $safeArgs = @($ExeArgs) + @('--no-reboot', '--no-explorer-restart')
    $p.StartInfo.Arguments = ($safeArgs | ForEach-Object {
        '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
    }) -join ' '
    $p.StartInfo.WorkingDirectory = $root
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardOutput = $true
    $p.StartInfo.RedirectStandardError = $true
    $p.StartInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
    $p.StartInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
    [void]$p.Start()
    $stdout = $p.StandardOutput.ReadToEndAsync()
    $stderr = $p.StandardError.ReadToEndAsync()
    if (-not $p.WaitForExit(30000)) { $p.Kill(); $p.Dispose(); throw 'WinKick exceeded the 30-second test timeout.' }
    $result = [pscustomobject]@{ Out = $stdout.GetAwaiter().GetResult(); Err = $stderr.GetAwaiter().GetResult(); Exit = $p.ExitCode }
    $p.Dispose()
    return $result
}

Write-Host "=== WinKick CLI tests ===" -ForegroundColor Cyan

# 1. version
$r = RunExe @('--version')
Check "--version matches VERSION" ($r.Out.Trim() -eq "WinKick $version")
Check "--version exit 0" ($r.Exit -eq 0)

# 2. help
$r = RunExe @('--help')
Check "--help lists --tags" ($r.Out -match '--tags')
Check "--help exit 0" ($r.Exit -eq 0)

# 3. list with test ini (guards visible)
$r = RunExe @('--config', $testIni, '--list')
Check "--list exit 0" ($r.Exit -eq 0)
Check "--list shows table header" ($r.Out -match 'Category')
Check "--list shows test item" ($r.Out -match 'Echo Greeting')
Check "--list win7 guard mismatch" ($r.Out -match 'win7[^\n]*os: mismatch|os: mismatch')

# 4. missing config -> explicit error, exit 2
$r = RunExe @('--config', 'does-not-exist-12345.ini', '--list')
Check "missing config exit 2" ($r.Exit -eq 2)
Check "missing config explicit error" (($r.Err + $r.Out) -match 'Cannot read config file')

# 5. offline net guard
$r = RunExe @('--offline', '--config', $testIni, '--list')
Check "offline --list exit 0" ($r.Exit -eq 0)
Check "offline --list net guard shown" ($r.Out -match 'net: offline')

# 6. dry-run default selection (group first-wins visible)
$r = RunExe @('--config', $testIni, '--dry-run')
Check "dry-run exit 0" ($r.Exit -eq 0)
Check "dry-run simulates default item" ($r.Out -match 'hello-winkick-test')
Check "dry-run: first radio wins" ($r.Out -match 'radio-a')
Check "dry-run: second radio NOT simulated" (-not ($r.Out -match 'radio-b'))
Check "dry-run: log header present" ($r.Out -match 'Mode: Dry-Run')

# 7. --all with --exclude group:radio
$r = RunExe @('--config', $testIni, '--dry-run', '--all', '--exclude', 'group:radio')
Check "--all+excl: radio-a not selected" (-not ($r.Out -match 'radio-a'))
Check "--all+excl: radio-b not selected" (-not ($r.Out -match 'radio-b'))

# 8. net tag selected but offline -> pipeline deselects (never executes)
$r = RunExe @('--config', $testIni, '--offline', '--dry-run', '--tags', 'net')
Check "offline net: nothing executed/simulated" (-not ($r.Out -match 'ping -n 1'))

# 9. gui-tagged item: --dry-run simulates it; --silent skips it.
$r = RunExe @('--config', $testIni, '--dry-run', '--tags', 'gui')
Check "gui item simulated in dry-run (not skipped)" ($r.Out -match 'start notepad\.exe')
Check "gui item not rejected in dry-run" (-not ($r.Out -match 'SKIPPED \(Interactive GUI required\)'))
$r = RunExe @('--config', $testIni, '--dry-run', '--silent', '--tags', 'gui')
Check "gui item skipped with --silent" ($r.Out -match 'SKIPPED \(Interactive GUI required\)')

# 10. OS guard via pipeline: win7 item filtered even with --all
$r = RunExe @('--config', $testIni, '--dry-run', '--all')
Check "win7 item never runs on Win11" (-not ($r.Out -match 'this-should-never-run-here'))
if ($isWin11) {
    Check "win11 item selected with --all" ($r.Out -match 'running-on-windows-11')
} else {
    Check "win11 item skipped on Win10 host (os: mismatch)" ((-not ($r.Out -match 'running-on-windows-11')) -and ($r.Out -match 'OS mismatch'))
}

# 11. --export resolves selection (tags, case-insensitive)
$exportPath = Join-Path $outDir 'export_tags.ini'
$r = RunExe @('--config', $testIni, '--export', $exportPath, '--tags', 'FAILURE-DEMO')
Check "--export exit 0" ($r.Exit -eq 0)
$exportContent = Get-Content $exportPath -Raw -ErrorAction SilentlyContinue
Check "export exists" ($null -ne $exportContent)
if ($exportContent) {
    Check "export: failure-demo checked" ($exportContent -match '(?s)Intentional Failure\]\s*\nparam = check:1')
    Check "export: greeting not checked" ($exportContent -match '(?s)Echo Greeting\]\s*\nparam = check:0')
}

# 12. --export encodes group first-wins from --all
$exportPath2 = Join-Path $outDir 'export_all.ini'
$r = RunExe @('--config', $testIni, '--export', $exportPath2, '--all')
$exportAll = Get-Content $exportPath2 -Raw -ErrorAction SilentlyContinue
if ($exportAll) {
    Check "export --all: Radio A checked" ($exportAll -match '(?s)Radio A\]\s*\nparam = check:1')
    Check "export --all: Radio B forced unchecked" ($exportAll -match '(?s)Radio B\]\s*\nparam = check:0')
} else { Check "export_all exists" $false }

# 13. --dump-config round-trip
$dumpPath = Join-Path $outDir 'dump.ini'
$r = RunExe @('--dump-config', $dumpPath)
Check "--dump-config exit 0" ($r.Exit -eq 0)
$dumpContent = Get-Content $dumpPath -Raw -ErrorAction SilentlyContinue
Check "dump written and non-trivial" (($dumpContent) -and $dumpContent.Length -gt 500)
Check "dump parses back (list works)" ((RunExe @('--config', $dumpPath, '--list')).Exit -eq 0)

# 14. malformed ini is tolerated (no crash, no partial garbage)
$badIni = Join-Path $outDir ('bad_tasks_' + [guid]::NewGuid().ToString('N') + '.ini')
Set-Content -Path $badIni -Value "random garbage line`n[Broken`nkey without value`n=orphan`n[Good | Fine]`nparam = check:1`ncmd = cmd /c echo survived" -Encoding utf8
$r = RunExe @('--config', $badIni, '--list')
Check "malformed ini: exit 0" ($r.Exit -eq 0)
Check "malformed ini: good item parsed" ($r.Out -match 'Good')
$r = RunExe @('--config', $badIni, '--dry-run')
Check "malformed ini: survived dry-run" ($r.Out -match 'survived|Only good item')
Remove-Item $badIni -Force -ErrorAction SilentlyContinue

# 15. --log writes a UTF-8 log file
$logPath = Join-Path $outDir 'log_test.txt'
Remove-Item $logPath -Force -ErrorAction SilentlyContinue
$r = RunExe @('--config', $testIni, '--dry-run', '--log', $logPath, '--tags', 'common')
Check "--dry-run --log exit 0" ($r.Exit -eq 0)
Check "log file created" (Test-Path $logPath)
if (Test-Path $logPath) {
    $log = Get-Content $logPath -Raw
    Check "log: mode is Dry-Run" ($log -match 'Mode: Dry-Run')
    Check "log: FAILED ITEMS section present" ($log -match 'FAILED ITEMS:')
}

# 16. Unknown flag -> exit 2 with message
$r = RunExe @('--definitely-not-a-flag')
Check "unknown flag exit 2" ($r.Exit -eq 2)
Check "unknown flag error text" ($r.Err -match 'Unknown flag')

# 17. Missing value for --config -> exit 2
$r = RunExe @('--config')
Check "--config missing value exit 2" ($r.Exit -eq 2)

# 18. --silent refused without admin (this machine is not elevated)
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
$isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
$r = RunExe @('--silent', '--config', $testIni, '--tags', 'common')
if ($isAdmin) {
    Check "--silent as admin exit 0" ($r.Exit -eq 0)
} else {
    Check "--silent without admin exit 1" ($r.Exit -eq 1)
    Check "--silent without admin message" ($r.Err -match 'Administrator privileges required')
}

# 19. --pick substring selection (case-insensitive) + {} substitution visible in dry-run
$r = RunExe @('--config', $testIni, '--dry-run', '--pick', 'robocopy')
Check "--pick selects robocopy item" ($r.Out -match 'robocopy')
Check "--pick: args substituted (MT:4)" ($r.Out -match '/MT:4')
Check "--pick: no placeholder remains" (-not ($r.Out -match '\{Threads\}'))

# 20. --all: win11 item selected (Win11 hosts) / skipped (Win10 hosts); reboot item dry-runs
$r = RunExe @('--config', $testIni, '--dry-run', '--all')
if ($isWin11) {
    Check "--all selects win11 item" ($r.Out -match 'running-on-windows-11')
} else {
    Check "--all: win11 item skipped on Win10 host" ((-not ($r.Out -match 'running-on-windows-11')) -and ($r.Out -match 'OS mismatch'))
}
Check "--all reboot item dry-runs (no real reboot)" ($r.Out -match 'reboot-demo')

Write-Host ""
Write-Host "=== Execution engine harness ===" -ForegroundColor Cyan
$harness = Join-Path $outDir 'runner_harness.exe'
$buildOk = $true
if (-not $SkipBuild) {
    cmd /c "`"$PSScriptRoot\build_harness.bat`""
    $buildOk = $LASTEXITCODE -eq 0
    Check "harness build exit 0" $buildOk
}
Check "harness built" (Test-Path $harness)
if ($buildOk -and (Test-Path $harness)) {
    $proc = New-Object Diagnostics.Process
    $proc.StartInfo.FileName = $harness
    $proc.StartInfo.WorkingDirectory = $root
    $proc.StartInfo.UseShellExecute = $false
    $proc.StartInfo.RedirectStandardOutput = $true
    $proc.StartInfo.RedirectStandardError = $true
    [void]$proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    if (-not $proc.WaitForExit(30000)) { $proc.Kill(); $proc.Dispose(); throw 'Harness exceeded the 30-second test timeout.' }
    $harnessOut = $outTask.GetAwaiter().GetResult()
    $harnessErr = $errTask.GetAwaiter().GetResult()
    Check "harness exit 0" ($proc.ExitCode -eq 0)
    Check "harness completed all scenarios" ($harnessOut -match 'ALL HARNESS SCENARIOS PASSED')
    $proc.Dispose()
    Write-Host $harnessOut
    if ($harnessErr) { Write-Host $harnessErr -ForegroundColor Red }
    foreach ($line in ($harnessOut -split "`n")) {
        if ($line -match '^\[PASS\]') { $script:pass++ }
        elseif ($line -match '^\[FAIL\]') { $script:fail++ }
    }
}

Write-Host ""
Write-Host ("PASS: {0}  FAIL: {1}" -f $script:pass, $script:fail) -ForegroundColor Cyan
if ($script:fail -eq 0) { exit 0 } else { exit 1 }
