# Syntax/structure checks only: NEVER invoke a task's command.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\scripts\common.ps1')
$path = Join-Path $script:RepoRoot 'res\tasks.ini'
$text = [IO.File]::ReadAllText($path, [Text.Encoding]::UTF8)
$blocks = @([regex]::Split($text, '(?m)(?=^\[)') | Where-Object { $_ -match '^\[' })
if ($blocks.Count -lt 1) { throw 'Empty task catalog.' }
$titles = @{}
$scripts = 0
foreach ($block in $blocks) {
    $title = [regex]::Match($block, '^\[([^\r\n]+)\]').Groups[1].Value
    if ($titles.ContainsKey($title)) { throw "Duplicate task: $title" }
    $titles[$title] = $true
    foreach ($key in @('param','tags','desc','cmd')) {
        if ([regex]::Matches($block, "(?m)^$key\s*=").Count -ne 1) { throw "Missing/duplicate $key in $title" }
    }
    if ($block -notmatch '(?m)^param\s*=\s*check:0\s*$') { throw "Shipped task must be opt-in: $title" }
    $cmd = [regex]::Match($block, '(?m)^cmd\s*=\s*(.+)$').Groups[1].Value.TrimEnd("`r")
    if ($cmd.Length -ge 8191) { throw "Command too long for cmd.exe: $title" }
    if (($cmd.ToCharArray() | Where-Object { $_ -eq '"' }).Count % 2) { throw "Unbalanced command quotes: $title" }
    foreach ($arg in [regex]::Matches($block, '(?m)^arg\.([^\s=]+)\s*=\s*(?:num|txt):(.*)$')) {
        $name = $arg.Groups[1].Value
        if (-not $cmd.Contains('{' + $name + '}')) { throw "Unused argument $name in $title" }
        $cmd = $cmd.Replace('{' + $name + '}', $arg.Groups[2].Value.TrimEnd("`r"))
    }
    $marker = ' -Command "'
    $offset = $cmd.IndexOf($marker)
    if ($offset -ge 0) {
        if (-not $cmd.EndsWith('"')) { throw "Unclosed PowerShell command: $title" }
        $body = $cmd.Substring($offset + $marker.Length, $cmd.Length - $offset - $marker.Length - 1)
        $tokens = $null; $errors = $null
        [void][Management.Automation.Language.Parser]::ParseInput($body, [ref]$tokens, [ref]$errors)
        if ($errors) { throw "${title}: $($errors -join '; ')" }
        $scripts++
    }
}
$list = Invoke-ReadOnlyWinKick @('--offline','--config',$path,'--list')
if ([regex]::Matches($list.Out, '(?m)^\d+\s+').Count -ne $blocks.Count) { throw 'Native parser lost catalog entries.' }
$dry = Invoke-ReadOnlyWinKick @('--offline','--config',$path,'--dry-run','--all')
if ($dry.Out -match 'RES: OK|RES: ERR|Reboot initiated|Restarting explorer') { throw 'Unexpected execution during dry-run.' }
$dumpPath = Join-Path $script:RepoRoot 'tests\out\embedded-catalog.ini'
[void](Invoke-ReadOnlyWinKick @('--dump-config', $dumpPath))
if ((Get-FileHash $dumpPath).Hash -ne (Get-FileHash $path).Hash) { throw 'Embedded catalog differs from reviewed res/tasks.ini.' }
Write-Output "Catalog: $($blocks.Count) opt-in tasks; $scripts PowerShell bodies parsed; native list/dry-run and exact embedded bytes verified."
