# Shared local/CI/release gates. No production task is ever applied.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Push-Location $root
try {
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\run_tests.ps1
    if ($LASTEXITCODE -ne 0) { throw 'CLI/engine tests failed.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\catalog_test.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Catalog validation failed.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\catalog_fragments_test.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Catalog fragment tests failed.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File tests\repository_test.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Git/release-policy tests failed.' }
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\verify-binary.ps1
    if ($LASTEXITCODE -ne 0) { throw 'Binary verification failed.' }
} finally { Pop-Location }
