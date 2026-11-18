# VERSION is the only manually maintained product version.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
if ($version -notmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?$') {
    throw 'VERSION must be MAJOR.MINOR.PATCH with an optional prerelease suffix.'
}
$parts = @([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
if (@($parts | Where-Object { $_ -gt 65535 }).Count) { throw 'Windows version components must be <= 65535.' }
$dir = Join-Path $root 'build\generated'
[void][IO.Directory]::CreateDirectory($dir)
$numeric = ($parts -join ',') + ',0'
$fileVersion = ($parts -join '.') + '.0'
$header = @"
// Generated from VERSION. Do not edit or commit this file.
#pragma once
#define WINKICK_VERSION_STRING "$version"
#define WINKICK_VERSION_WSTRING L"$version"
#define WINKICK_FILE_VERSION $numeric
#define WINKICK_FILE_VERSION_STRING "$fileVersion"
"@
[IO.File]::WriteAllText((Join-Path $dir 'version.h'), $header + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Output "Version: $version"
