$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$compiler = Get-Command cl.exe -CommandType Application -ErrorAction Stop
$data = [ordered]@{
    CompilerFileVersion = (Get-Item $compiler.Source).VersionInfo.FileVersion
    VCToolsVersion = $env:VCToolsVersion
    WindowsSDKVersion = $env:WindowsSDKVersion
    VisualStudioVersion = $env:VisualStudioVersion
    RunnerImage = $env:ImageOS
    RunnerImageVersion = $env:ImageVersion
}
$json = $data | ConvertTo-Json
[IO.File]::WriteAllText((Join-Path $root 'build\toolchain.json'), $json + "`n", (New-Object Text.UTF8Encoding($false)))
