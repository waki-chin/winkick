param([string]$Executable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'common.ps1')
if (-not $Executable) { $Executable = Join-Path $script:RepoRoot 'build\winkick.exe' }
$Executable = [IO.Path]::GetFullPath($Executable)
$bytes = [IO.File]::ReadAllBytes($Executable)
function U16([int]$offset) { [BitConverter]::ToUInt16($bytes, $offset) }
function U32([int]$offset) { [BitConverter]::ToUInt32($bytes, $offset) }
if ((U16 0) -ne 0x5A4D) { throw 'Not an MZ executable.' }
$pe = [int](U32 0x3C)
if ((U32 $pe) -ne 0x4550 -or (U16 ($pe+4)) -ne 0x8664) { throw 'Not an x64 PE image.' }
$opt = $pe + 24
if ((U16 $opt) -ne 0x20B -or (U16 ($opt+68)) -ne 2) { throw 'Expected PE32+ Windows GUI subsystem.' }
$flags = U16 ($opt+70)
if (($flags -band 0x4160) -ne 0x4160) { throw 'ASLR, high-entropy VA, DEP, and CFG must be enabled.' }
$sections = @()
$sectionTable = $opt + (U16 ($pe+20))
for ($i=0; $i -lt (U16 ($pe+6)); $i++) {
    $s = $sectionTable + 40*$i
    $sections += [pscustomobject]@{ RVA=(U32 ($s+12)); Size=[Math]::Max((U32 ($s+8)),(U32 ($s+16))); Raw=(U32 ($s+20)) }
}
function Offset([uint32]$rva) {
    foreach ($s in $sections) {
        if ($rva -ge $s.RVA -and $rva -lt ($s.RVA+$s.Size)) {
            $n = [int]($s.Raw+$rva-$s.RVA)
            if ($n -lt 0 -or $n -ge $bytes.Length) { throw 'Invalid PE offset.' }
            return $n
        }
    }
    throw "Unmapped PE RVA: $rva"
}
function DllName([uint32]$rva) {
    $n = Offset $rva
    $end = $n
    while ($end -lt $bytes.Length -and $bytes[$end] -ne 0 -and $end-$n -lt 260) { $end++ }
    if ($end -ge $bytes.Length -or $end-$n -ge 260) { throw 'Invalid import name.' }
    return [Text.Encoding]::ASCII.GetString($bytes,$n,$end-$n).ToLowerInvariant()
}
function Imports([int]$directory, [int]$stride, [int]$nameField) {
    $rva = U32 ($opt+112+8*$directory)
    $size = U32 ($opt+116+8*$directory)
    if (-not $rva) { return @() }
    $start = Offset $rva
    $result = @()
    for ($i=0; $i -lt [Math]::Floor($size/$stride); $i++) {
        $entry = $start+$stride*$i
        $name = U32 ($entry+$nameField)
        if (-not $name) { break }
        if ($directory -eq 13 -and (U32 $entry) -ne 1) { throw 'Expected RVA-based delay import descriptor.' }
        $result += DllName $name
    }
    return $result
}
$static = @(Imports 1 20 12)
$delayed = @(Imports 13 32 4)
$allowStatic = @('kernel32.dll','user32.dll','gdi32.dll','comctl32.dll','shell32.dll','comdlg32.dll','advapi32.dll','ole32.dll')
if (-not $static.Count -or @($static | Where-Object { $_ -notin $allowStatic }).Count) {
    throw "Unexpected static dependency (or non-static CRT): $($static -join ', ')"
}
if (@(Compare-Object @('winhttp.dll','wininet.dll','bcrypt.dll') ($delayed | Sort-Object)).Count) {
    throw "Unexpected delay-load table: $($delayed -join ', ')"
}
if (-not ('WinKickResourceReader' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WinKickResourceReader {
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    static extern IntPtr FindResource(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll")] static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern IntPtr LockResource(IntPtr data);
    [DllImport("kernel32.dll")] static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")] static extern bool FreeLibrary(IntPtr module);
    public static byte[] Read(string path, int id, int type) {
        // DATAFILE + IMAGE_RESOURCE: never execute code or load the EXE's imports.
        IntPtr h=LoadLibraryEx(path,IntPtr.Zero,0x22);
        if(h==IntPtr.Zero) throw new Exception("Could not map resources.");
        try {
            IntPtr r=FindResource(h,(IntPtr)id,(IntPtr)type);
            if(r==IntPtr.Zero) throw new Exception("Missing embedded resource.");
            uint size=SizeofResource(h,r);
            IntPtr p=LockResource(LoadResource(h,r));
            if(p==IntPtr.Zero || size==0 || size>16*1024*1024) throw new Exception("Invalid resource.");
            byte[] data=new byte[(int)size]; Marshal.Copy(p,data,0,data.Length); return data;
        } finally { FreeLibrary(h); }
    }
}
'@
}
$manifest = [Text.Encoding]::UTF8.GetString([WinKickResourceReader]::Read($Executable,1,24))
$doc = New-Object Xml.XmlDocument
$doc.XmlResolver = $null
if ($manifest -match '<!DOCTYPE') { throw 'Unexpected manifest DTD.' }
$doc.LoadXml($manifest.TrimStart([char]0xFEFF))
if (-not $doc.SelectSingleNode("//*[local-name()='requestedExecutionLevel' and @level='asInvoker' and @uiAccess='false']")) { throw 'Manifest must be asInvoker without UIAccess.' }
if (-not $doc.SelectSingleNode("//*[local-name()='assemblyIdentity' and @name='Microsoft.Windows.Common-Controls' and @version='6.0.0.0']")) { throw 'Missing Common Controls v6 SxS dependency.' }
if (-not $doc.SelectSingleNode("//*[local-name()='dpiAwareness' and text()='PerMonitorV2']")) { throw 'Missing PerMonitorV2 manifest.' }
$ids = [IO.File]::ReadAllText((Join-Path $script:RepoRoot 'res\resource_ids.h'))
$id = [int][regex]::Match($ids, '#define\s+IDR_DEFAULT_TASKS\s+(\d+)').Groups[1].Value
$embedded = [WinKickResourceReader]::Read($Executable,$id,10)
$hash = [Security.Cryptography.SHA256]::Create()
try {
    $actual = [BitConverter]::ToString($hash.ComputeHash($embedded)).Replace('-','')
} finally { $hash.Dispose() }
if ($actual -ne (Get-FileHash (Join-Path $script:RepoRoot 'res\tasks.ini')).Hash) { throw 'Embedded task catalog mismatch.' }
$version = Get-WinKickVersion
if ([Diagnostics.FileVersionInfo]::GetVersionInfo($Executable).ProductVersion -ne $version) { throw 'PE product version differs from VERSION.' }
$cli = Invoke-ReadOnlyWinKick -Executable $Executable -Arguments @('--version')
if ($cli.Out.Trim() -ne "WinKick $version") { throw 'CLI version differs from VERSION.' }
Write-Output "Binary verified: x64 GUI, ASLR/DEP/CFG, static CRT imports, delay-load policy, manifest, version $version, embedded catalog SHA-256 $actual"
