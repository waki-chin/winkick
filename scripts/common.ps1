$ErrorActionPreference = 'Stop'
$script:RepoRoot = Split-Path -Parent $PSScriptRoot

function Get-WinKickVersion {
    $v = [IO.File]::ReadAllText((Join-Path $script:RepoRoot 'VERSION')).Trim()
    if ($v -notmatch '^\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'Invalid VERSION.' }
    return $v
}

function Invoke-ReadOnlyWinKick {
    param([string[]]$Arguments, [string]$Executable = (Join-Path $script:RepoRoot 'build\winkick.exe'))
    # Defense against accidentally applying the real setup catalog from a release check.
    $readOnly = @('--help','--version','--list','--dump-config','--export','--dry-run')
    if (-not @($Arguments | Where-Object { $_ -in $readOnly }).Count) {
        throw 'A read-only WinKick output mode is required.'
    }
    $p = New-Object Diagnostics.Process
    try {
        $p.StartInfo.FileName = [IO.Path]::GetFullPath($Executable)
        $p.StartInfo.WorkingDirectory = $script:RepoRoot
        $safe = @($Arguments) + @('--no-reboot','--no-explorer-restart')
        $p.StartInfo.Arguments = ($safe | ForEach-Object {
            '"' + [regex]::Replace([regex]::Replace($_, '(\\*)"', '$1$1\"'), '(\\+)$', '$1$1') + '"'
        }) -join ' '
        $p.StartInfo.UseShellExecute = $false
        $p.StartInfo.RedirectStandardOutput = $true
        $p.StartInfo.RedirectStandardError = $true
        $p.StartInfo.StandardOutputEncoding = [Text.Encoding]::UTF8
        $p.StartInfo.StandardErrorEncoding = [Text.Encoding]::UTF8
        [void]$p.Start()
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(45000)) { $p.Kill(); throw 'WinKick verification timed out.' }
        $out = $stdout.GetAwaiter().GetResult()
        $err = $stderr.GetAwaiter().GetResult()
        if ($p.ExitCode -ne 0) { throw "WinKick exited $($p.ExitCode): $err" }
        return [pscustomobject]@{ Out=$out; Err=$err; Exit=$p.ExitCode }
    } finally { $p.Dispose() }
}
