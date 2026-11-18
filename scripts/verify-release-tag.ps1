param([Parameter(Mandatory=$true)][string]$Tag)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if ($Tag -notmatch '^v\d+\.\d+\.\d+(?:-[0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*)?$') { throw 'Expected a vMAJOR.MINOR.PATCH tag.' }
if ($Tag -ne ('v' + (Get-WinKickVersion))) { throw 'Release tag must match VERSION exactly.' }
$head = & git -C $script:RepoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $head -notmatch '^[0-9a-f]{40}$') { throw 'Cannot resolve source commit.' }
$tagCommit = & git -C $script:RepoRoot rev-parse --verify ("refs/tags/$Tag" + '^{commit}')
if ($LASTEXITCODE -ne 0 -or $tagCommit -ne $head) { throw 'Checkout does not match the exact tagged commit.' }
& git -C $script:RepoRoot merge-base --is-ancestor HEAD refs/remotes/origin/main
if ($LASTEXITCODE -ne 0) { throw 'Release commit must already be merged into origin/main.' }
if ($env:GITHUB_SHA -and $env:GITHUB_SHA -ne $head) { throw 'GitHub event SHA differs from the checked-out source.' }
$dirty = & git -C $script:RepoRoot status --porcelain --untracked-files=normal
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Release requires a clean source checkout.' }
Write-Output "Release source verified: $Tag at $head"
