param([string]$ReleaseTag = '')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if ($ReleaseTag) {
    & (Join-Path $PSScriptRoot 'verify-release-tag.ps1') -Tag $ReleaseTag
}
& (Join-Path $PSScriptRoot 'verify-binary.ps1')
$version = Get-WinKickVersion
$dist = Join-Path $script:RepoRoot 'dist'
$stage = Join-Path $script:RepoRoot 'build\package'
foreach ($dir in @($dist,$stage)) {
    if (Test-Path $dir) { throw "Refusing to mix stale assets: move or remove generated directory $dir before packaging." }
}
$commit = $null
$dirty = $true
if (Test-Path (Join-Path $script:RepoRoot '.git')) {
    $commit = & git -C $script:RepoRoot rev-parse HEAD
    if ($LASTEXITCODE -ne 0) { throw 'Cannot resolve Git commit.' }
    $status = & git -C $script:RepoRoot status --porcelain --untracked-files=normal
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect Git checkout.' }
    $dirty = [bool]$status
}
if ($ReleaseTag -and ($dirty -or $commit -notmatch '^[0-9a-f]{40}$')) { throw 'Refusing an uncommitted/dirty release package.' }
foreach ($dir in @($dist,$stage)) { [void][IO.Directory]::CreateDirectory($dir) }
Copy-Item (Join-Path $script:RepoRoot 'build\winkick.exe') (Join-Path $dist 'winkick.exe')
Copy-Item (Join-Path $script:RepoRoot 'res\tasks.ini') (Join-Path $dist 'tasks.ini')
$toolchain = Get-Content (Join-Path $script:RepoRoot 'build\toolchain.json') -Raw | ConvertFrom-Json
$info = [ordered]@{
    Product='WinKick'; Version=$version; Architecture='x64'; Tag=$ReleaseTag
    SourceCommit=$commit; DirtySource=$dirty
    BuiltAtUtc=[DateTime]::UtcNow.ToString('o')
    Toolchain=$toolchain
    RunnerOS=$env:RUNNER_OS; Workflow=$env:GITHUB_WORKFLOW
    RunId=$env:GITHUB_RUN_ID; RunAttempt=$env:GITHUB_RUN_ATTEMPT
    Repository=$env:GITHUB_REPOSITORY
    ExecutableSHA256=(Get-FileHash (Join-Path $dist 'winkick.exe')).Hash.ToLowerInvariant()
    TaskCatalogSHA256=(Get-FileHash (Join-Path $dist 'tasks.ini')).Hash.ToLowerInvariant()
    Checks='Executable format, imports, manifest, version and embedded catalog verified during packaging. Full tests are run separately by the maintainer.'
    AuthenticodeSigned=$false
}
[IO.File]::WriteAllText((Join-Path $dist 'build-info.json'), ($info | ConvertTo-Json -Depth 6) + "`n", (New-Object Text.UTF8Encoding($false)))
foreach ($name in @('winkick.exe','tasks.ini','build-info.json')) { Copy-Item (Join-Path $dist $name) $stage }
foreach ($name in @('README.md','LICENSE','SECURITY.md','CHANGELOG.md')) { Copy-Item (Join-Path $script:RepoRoot $name) $stage }
$stageDocs = Join-Path $stage 'docs'
[void][IO.Directory]::CreateDirectory($stageDocs)
Copy-Item (Join-Path $script:RepoRoot 'docs\tasks.md') $stageDocs
$zip = Join-Path $dist "winkick-v$version-windows-x64.zip"
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $zip -CompressionLevel Optimal
$lines = @(Get-ChildItem $dist -File | Sort-Object Name | ForEach-Object {
    (Get-FileHash $_.FullName).Hash.ToLowerInvariant() + '  ' + $_.Name
})
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS'), ($lines -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output "Package ready in dist/ (GitHub provenance is added only by the release workflow)."
