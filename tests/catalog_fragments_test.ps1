$ErrorActionPreference = 'Stop'
$iniPath = Join-Path $PSScriptRoot '..\res\tasks.ini'
$text = [IO.File]::ReadAllText($iniPath)
$blocks = @([regex]::Split($text, '(?m)(?=^\[)') | Where-Object { $_ -match '^\[' })
function TaskScript([string]$title) {
    $b = $blocks | Where-Object { $_.StartsWith('[' + $title + ']') }
    if (@($b).Count -ne 1) { throw "Missing/ambiguous task: $title" }
    $cmd = [regex]::Match($b, '(?m)^cmd\s*=\s*(.+)$').Groups[1].Value.TrimEnd("`r")
    $marker = ' -Command "'
    $offset = $cmd.IndexOf($marker)
    return $cmd.Substring($offset + $marker.Length, $cmd.Length - $offset - $marker.Length - 1)
}
$sample = TaskScript 'Applications | Firefox'
$start = $sample.IndexOf('$wg=Join-Path')
$end = $sample.IndexOf('& $wg ')
$resolver = $sample.Substring($start, $end - $start)

# No installed WinGet dependency: all resolution scenarios below are mocked.
$script:pass = 0
function Check([string]$name, [bool]$ok) {
    if (-not $ok) { throw "FAIL: $name" }
    $script:pass++
    Write-Output "[PASS] $name"
}
$aliasPath = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\winget.exe'
$pathExe = 'C:\Fake\PATH\winget.exe'
$latest = 'C:\Fake\WinGet-1.10\winget.exe'

# All mocks are local to this test process; no files, packages, features or
# registry keys are created/modified by these cases.
function Get-Command {
    [CmdletBinding()] param([Parameter(Position=0)][string]$Name, [string]$CommandType)
    $script:pathLookups++
    if ($script:scenario.PathExe) { [pscustomobject]@{ Source=$script:scenario.PathExe } }
}
function Get-AppxPackage {
    [CmdletBinding()] param([Parameter(Position=0)][string]$Name, [switch]$AllUsers)
    $script:packageLookups++
    if (-not $AllUsers) { throw 'Resolver did not request all users.' }
    if ($script:scenario.DenyPackages) { Write-Error 'Access denied in mock package lookup'; return }
    $script:scenario.Packages
}
function Test-Path {
    [CmdletBinding()] param([Parameter(Position=0)][string]$Path, [string]$LiteralPath, [string]$PathType)
    if (-not $LiteralPath) { $LiteralPath=$Path }
    if ($LiteralPath -like '*Component Based Servicing\RebootPending') { return [bool]$script:scenario.Pending }
    return $LiteralPath -in $script:scenario.Exists
}
function ResolveCase($case) {
    $script:scenario = $case
    $script:pathLookups=0
    $script:packageLookups=0
    & ([scriptblock]::Create($resolver + '; Write-Output $wg'))
}
$result = ResolveCase @{ Exists=@($aliasPath); PathExe=$pathExe }
Check 'App Execution Alias wins without PATH/package enumeration' ($result -eq $aliasPath -and $script:pathLookups -eq 0 -and $script:packageLookups -eq 0)
$result = ResolveCase @{ Exists=@($pathExe); PathExe=$pathExe }
Check 'PATH fallback works without package enumeration' ($result -eq $pathExe -and $script:packageLookups -eq 0)
$packages = @(
    [pscustomobject]@{ Version='1.9.0.0'; InstallLocation='C:\Fake\WinGet-1.9' },
    [pscustomobject]@{ Version='1.10.0.0'; InstallLocation='C:\Fake\WinGet-1.10' },
    [pscustomobject]@{ Version='99.0.0.0'; InstallLocation='' }
)
$result = ResolveCase @{ Exists=@($latest); Packages=$packages }
Check 'all-users fallback sorts numerically and ignores empty locations' ($result -eq $latest -and $script:packageLookups -eq 1)
foreach ($case in @(
    @{ Exists=@(); Packages=@() },
    @{ Exists=@(); Packages=@([pscustomobject]@{Version='1.0';InstallLocation=''}) },
    @{ Exists=@(); Packages=$packages },
    @{ Exists=@(); DenyPackages=$true }
)) {
    $friendly=$false
    try { ResolveCase $case | Out-Null }
    catch { $friendly=$_.Exception.Message -like '*WinGet is not installed. Run the bootstrap task first.*' }
    Check 'missing/inaccessible WinGet gives the explicit bootstrap error' $friendly
}

function Get-ItemProperty {
    [CmdletBinding()] param([Parameter(Position=0)][string]$Path)
    [pscustomobject]@{ CurrentBuildNumber='19044' }
}
function Get-WindowsOptionalFeature {
    [CmdletBinding()] param([switch]$Online, [string]$FeatureName)
    [pscustomobject]@{ State=$script:scenario.States[$FeatureName] }
}
function Enable-WindowsOptionalFeature {
    [CmdletBinding()] param([switch]$Online, [string]$FeatureName, [switch]$All, [switch]$NoRestart)
    if (-not $NoRestart) { throw 'Unsafe feature call without -NoRestart.' }
    $script:featureEnables++
    $script:scenario.States[$FeatureName]='EnablePending'
    [pscustomobject]@{ RestartNeeded=$true }
}
$subsystem='Microsoft-Windows-Subsystem-Linux'
$platform='VirtualMachinePlatform'
$stage1=TaskScript 'Windows features | Enable WSL prerequisites (stage 1)'
$stage2=TaskScript 'Applications | WSL (without a Linux distribution)'
$stage2guard=$stage2.Substring(0,$stage2.IndexOf('$wg=Join-Path'))
$script:scenario=@{ States=@{$subsystem='Disabled';$platform='Disabled'} }
$script:featureEnables=0
$output=& ([scriptblock]::Create($stage1))
Check 'WSL prerequisites only stage features and request manual restart' ($script:featureEnables -eq 2 -and $output -match 'Restart Windows manually')
$script:featureEnables=0
$output=& ([scriptblock]::Create($stage1))
Check 'already-pending WSL prerequisites are not re-enabled' ($script:featureEnables -eq 0 -and $output -match 'Restart Windows manually')
foreach($state in @('Disabled','EnablePending')) {
    $script:scenario=@{ States=@{$subsystem='Enabled';$platform=$state} }
    $blocked=$false
    try { & ([scriptblock]::Create($stage2guard)) }
    catch { $blocked=$_.Exception.Message -like '*WSL prerequisite not active*' }
    Check "WSL install blocks $state prerequisite state before reaching WinGet" $blocked
}
$script:scenario=@{ States=@{$subsystem='Enabled';$platform='Enabled'}; Pending=$true }
$blocked=$false
try { & ([scriptblock]::Create($stage2guard)) }
catch { $blocked=$_.Exception.Message -like '*component servicing requires a restart*' }
Check 'WSL install blocks a pending component-servicing restart' $blocked
$script:scenario.Pending=$false
& ([scriptblock]::Create($stage2guard))
Check 'WSL active prerequisites and no pending restart pass the read-only guard' $true

$tails=@()
foreach($title in @(
    'Applications | Visual Studio Build Tools (C++)',
    'Applications | CMake',
    'Applications | WSL (without a Linux distribution)',
    'Applications | Visual C++ Redistributable 2015+ (x64)',
    'Applications | Visual C++ Redistributable 2015+ (x86)'
)) {
    $body=TaskScript $title
    $tails += $body.Substring($body.IndexOf('$code=$LASTEXITCODE'))
}
Check 'all five reboot-tagged installer completion handlers match' (@($tails | Select-Object -Unique).Count -eq 1)
foreach($code in @(0,3010,1603,7)) {
    # Only the extracted result handler runs; no actual WinGet/install command.
    $body='$ProgressPreference=''SilentlyContinue''; $LASTEXITCODE='+$code+'; '+$tails[0]
    $encoded=[Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($body))
    $p=New-Object Diagnostics.Process
    $p.StartInfo.FileName=Join-Path $PSHOME 'powershell.exe'
    $p.StartInfo.Arguments='-NoProfile -NonInteractive -EncodedCommand '+$encoded
    $p.StartInfo.UseShellExecute=$false
    $p.StartInfo.RedirectStandardOutput=$true
    $p.StartInfo.RedirectStandardError=$true
    [void]$p.Start()
    $out=$p.StandardOutput.ReadToEndAsync()
    $err=$p.StandardError.ReadToEndAsync()
    if(-not $p.WaitForExit(10000)) { $p.Kill(); throw 'Result-handler test timed out.' }
    $stdout=$out.GetAwaiter().GetResult()
    $stderr=$err.GetAwaiter().GetResult()
    $expected=$code
    if($code -eq 3010) { $expected=0 }
    Check "installer result $code maps to exit $expected" ($p.ExitCode -eq $expected -and -not $stderr)
    if($code -eq 3010) { Check '3010 normalization retains a visible reboot notice' ($stdout -like '*restart required (3010)*') }
    $p.Dispose()
}
Write-Output "Fragment checks passed: $script:pass"
