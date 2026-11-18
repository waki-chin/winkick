# Same checkout/toolchain/environment, two fresh compiler invocations.
# This does not claim reproducibility across different MSVC/SDK/runner versions.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $root 'build\winkick.exe'
$first = (Get-FileHash $exe -Algorithm SHA256).Hash
Push-Location $root
try {
    & cmd.exe /d /c build.bat
    if ($LASTEXITCODE -ne 0) { throw 'Repeat build failed.' }
} finally { Pop-Location }
$second = (Get-FileHash $exe -Algorithm SHA256).Hash
if ($first -ne $second) { throw "Non-repeatable EXE: $first versus $second" }
Write-Output "Repeat-build SHA-256 matches: $second"
