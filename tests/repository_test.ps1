# Exercise Git policy and release rejection paths in a disposable repository only.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path $PSScriptRoot ('out\repository-' + [guid]::NewGuid().ToString('N'))
[void][IO.Directory]::CreateDirectory((Join-Path $fixture 'scripts'))
foreach ($name in @('.gitignore','.gitattributes','VERSION')) { Copy-Item (Join-Path $root $name) $fixture }
foreach ($name in @('common.ps1','verify-release-tag.ps1')) { Copy-Item (Join-Path $root "scripts\$name") (Join-Path $fixture 'scripts') }
$script:checks = 0
function Check([string]$Name, [bool]$Condition) {
    if (-not $Condition) { throw "Repository check failed: $Name" }
    $script:checks++
    Write-Output "[PASS] $Name"
}
function Git {
    $output = & git.exe -C $fixture @args
    if ($LASTEXITCODE -ne 0) { throw "Fixture Git command failed: $args" }
    return $output
}
function Verify([string]$Tag, [bool]$ShouldPass, [string]$Name) {
    $p = New-Object Diagnostics.Process
    $p.StartInfo.FileName = 'powershell.exe'
    $p.StartInfo.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + (Join-Path $fixture 'scripts\verify-release-tag.ps1') + '" -Tag "' + $Tag + '"'
    $p.StartInfo.UseShellExecute = $false
    $p.StartInfo.RedirectStandardOutput = $true
    $p.StartInfo.RedirectStandardError = $true
    try {
        [void]$p.Start()
        $stdout = $p.StandardOutput.ReadToEndAsync()
        $stderr = $p.StandardError.ReadToEndAsync()
        if (-not $p.WaitForExit(30000)) { $p.Kill(); throw 'Release verification timed out.' }
        $passed = $p.ExitCode -eq 0
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
    } finally { $p.Dispose() }
    Check $Name ($passed -eq $ShouldPass)
    if ($ShouldPass) { $output.Trim() | Write-Output }
}
$oldSHA = $env:GITHUB_SHA
try {
    [void](Git init --quiet -b main)
    [void](Git config user.name 'WinKick fixture')
    [void](Git config user.email 'fixture@example.invalid')
    [void](Git config commit.gpgsign false)
    [void](Git config core.autocrlf false)
    [void](Git config core.safecrlf false)
    [void](Git add .)
    [void](Git commit --quiet -m fixture)
    $head = Git rev-parse HEAD
    [void](Git update-ref refs/remotes/origin/main $head)
    $version = [IO.File]::ReadAllText((Join-Path $fixture 'VERSION')).Trim()
    $tag = "v$version"
    [void](Git -c tag.gpgsign=false tag -a $tag -m fixture)
    $env:GITHUB_SHA = $head
    Verify $tag $true 'matching clean annotated tag accepted'
    Verify 'v999.0.0' $false 'tag/version mismatch rejected'
    Verify 'main' $false 'non-version ref rejected'
    $env:GITHUB_SHA = '0' * 40
    Verify $tag $false 'event/source SHA mismatch rejected'
    $env:GITHUB_SHA = $head
    [IO.File]::WriteAllText((Join-Path $fixture 'untracked.txt'), 'not reviewed')
    Verify $tag $false 'untracked public file rejected'
    Remove-Item -LiteralPath (Join-Path $fixture 'untracked.txt')
    [IO.File]::AppendAllText((Join-Path $fixture 'VERSION'), "`n")
    Verify $tag $false 'modified tracked file rejected'
    Copy-Item (Join-Path $root 'VERSION') (Join-Path $fixture 'VERSION') -Force
    [void][IO.Directory]::CreateDirectory((Join-Path $fixture 'build'))
    [IO.File]::WriteAllText((Join-Path $fixture 'build\generated.txt'), 'ignored output')
    Verify $tag $true 'ignored build output does not dirty source'
    foreach ($path in @('build/winkick.exe','dist/SHA256SUMS','tests/out/evidence.txt','.local/history/report.md','secret.pfx','.env','log.txt','tasks.ini')) {
        & git.exe -C $fixture check-ignore --quiet --no-index -- $path
        Check "ignored: $path" ($LASTEXITCODE -eq 0)
    }
    foreach ($path in @('res/tasks.ini','tests/test_tasks.ini','src/main.cpp','res/app.ico','README.md','.github/workflows/release.yml')) {
        & git.exe -C $fixture check-ignore --quiet --no-index -- $path
        Check "included: $path" ($LASTEXITCODE -eq 1)
    }
    $attributes = @(Git check-attr eol diff '--' build.bat scripts/common.ps1 res/tasks.ini src/main.cpp res/app.ico)
    foreach ($expected in @('build.bat: eol: crlf','scripts/common.ps1: eol: crlf','res/tasks.ini: eol: lf','src/main.cpp: eol: lf','res/app.ico: diff: unset')) {
        Check "attribute: $expected" ($expected -in $attributes)
    }
    [void](Git update-ref -d refs/remotes/origin/main)
    Verify $tag $false 'missing reviewed-main reference rejected'
    [void](Git update-ref refs/remotes/origin/main $head)
    [void](Git checkout --quiet --orphan unrelated)
    [IO.File]::WriteAllText((Join-Path $fixture 'unrelated.txt'), 'separate history')
    [void](Git add unrelated.txt)
    [void](Git commit --quiet -m unrelated)
    $unrelated = Git rev-parse HEAD
    [void](Git checkout --quiet main)
    [void](Git update-ref refs/remotes/origin/main $unrelated)
    Verify $tag $false 'release outside reviewed-main history rejected'
    [void](Git update-ref refs/remotes/origin/main $head)
    [IO.File]::WriteAllText((Join-Path $fixture 'next.txt'), 'next commit')
    [void](Git add next.txt)
    [void](Git commit --quiet -m next)
    $env:GITHUB_SHA = Git rev-parse HEAD
    Verify $tag $false 'checkout different from tagged commit rejected'
    Write-Output "Repository checks passed: $script:checks (fixture: $fixture)"
} finally { $env:GITHUB_SHA = $oldSHA }
