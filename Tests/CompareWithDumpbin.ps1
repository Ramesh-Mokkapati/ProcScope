<#
.SYNOPSIS
    Checks the reader classes against Microsoft's dumpbin, which is the reference for what a
    PE file or library exports and imports.

.DESCRIPTION
    Builds the same canonical listing from "ReaderTests dump" and from dumpbin output and fails on
    any difference. Covers 64-bit and 32-bit DLLs, forwarders, ordinal-only exports, delay-loaded
    imports, and import and static .lib files.
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release',
    [ValidateSet('x64', 'x86')]       [string] $Platform = 'x64',
    [string[]] $Files
)

$ErrorActionPreference = 'Stop'

$platformDir = if ($Platform -eq 'x86') { 'Win32' } else { 'x64' }
$harness = Join-Path $PSScriptRoot "..\bin\$platformDir\$Configuration\ReaderTests.exe"
if (-not (Test-Path $harness)) { throw "ReaderTests.exe not found at $harness - build it first." }

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -version '[17,18)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$toolset = Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt"
$dumpbin = "$vs\VC\Tools\MSVC\$($toolset.Trim())\bin\Hostx64\x64\dumpbin.exe"
if (-not (Test-Path $dumpbin)) { throw "dumpbin not found at $dumpbin" }

if (-not $Files) {
    $sdkLib = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\Lib" -Directory | Sort-Object Name | Select-Object -Last 1
    $msvcLib = "$vs\VC\Tools\MSVC\$($toolset.Trim())\lib\x64"
    $Files = @(
        "$env:WINDIR\System32\kernel32.dll",     # forwarders
        "$env:WINDIR\System32\ntdll.dll",        # no imports
        "$env:WINDIR\System32\shell32.dll",      # ordinal-only exports, delay imports
        "$env:WINDIR\System32\user32.dll",
        "$env:WINDIR\System32\ole32.dll",
        "$env:WINDIR\System32\msvcrt.dll",
        "$env:WINDIR\SysWOW64\kernel32.dll",     # PE32 read from a 64-bit process
        "$env:WINDIR\SysWOW64\shell32.dll",
        "$env:WINDIR\SysWOW64\user32.dll",
        "$env:WINDIR\System32\drivers\ntfs.sys",
        "$($sdkLib.FullName)\um\x64\kernel32.lib",  # import library
        "$($sdkLib.FullName)\um\x86\user32.lib",
        "$msvcLib\libcmt.lib",                      # static library
        "$msvcLib\oldnames.lib"
    )
}

function Get-DumpbinListing([string] $file) {
    $lines = [System.Collections.Generic.List[string]]::new()
    if ($file -match '\.(lib|a)$') {
        # The second linker member lists "offset-index  symbol" once for each public symbol.
        $inSecond = $false
        $count = 0
        foreach ($line in (& $dumpbin /nologo /linkermember:2 $file)) {
            if ($line -match '^\s*\d+ public symbols') { $inSecond = $true; continue }
            if ($inSecond -and $line -match '^(\s*Summary|Archive member name)') { break }
            if ($inSecond -and $line -match '^\s+([0-9A-Fa-f]+)\s+(\S+)\s*$') { $lines.Add("S $($Matches[2])"); $count++ }
        }
        return $lines
    }

    $mode = ''
    $dll = ''
    $delay = $false
    foreach ($line in (& $dumpbin /nologo /exports /imports $file)) {
        if ($line -match '^\s+ordinal\s+hint\s+RVA\s+name') { $mode = 'exports'; continue }
        if ($line -match '^\s+Section contains the following imports') { $mode = 'imports'; $delay = $false; continue }
        if ($line -match '^\s+Section contains the following delay load imports') { $mode = 'imports'; $delay = $true; continue }
        if ($line -match '^\s+Summary') { $mode = '' }

        if ($mode -eq 'exports') {
            if ($line -match '^\s+(\d+)\s+[0-9A-F]+\s+([0-9A-F]{8})\s+(\S.*?)\s*$') {
                $lines.Add(('E {0} {1} {2} ' -f $Matches[1], ($Matches[3] -replace '\s+=.*$', ''), $Matches[2]).TrimEnd() + ' ')
            }
            elseif ($line -match '^\s+(\d+)\s+[0-9A-F]+\s+(\S+)\s+\(forwarded to (\S+)\)') {
                $lines.Add("E $($Matches[1]) $($Matches[2]) 00000000 $($Matches[3])")
            }
            elseif ($line -match '^\s+(\d+)\s+\[NONAME\]\s+\(forwarded to (\S+)\)') {
                $lines.Add("E $($Matches[1]) [NONAME] 00000000 $($Matches[2])")
            }
            elseif ($line -match '^\s+(\d+)\s+([0-9A-F]{8})\s+\[NONAME\]') {
                $lines.Add("E $($Matches[1]) [NONAME] $($Matches[2]) ")
            }
        }
        elseif ($mode -eq 'imports') {
            if ($line -match '^\s{4}(\S+\.\S+)\s*$') { $dll = $Matches[1].ToLowerInvariant(); continue }
            $tag = if ($delay) { 'D' } else { 'I' }
            if ($line -match '^\s+(?:[0-9A-F]{8,16}\s+)?Ordinal\s+(\d+)\s*$') { $lines.Add("$tag $dll #$($Matches[1])") }
            elseif ($delay) {
                # Delay-load function rows are "<thunk address> <hint> <name>"; other rows are table headers.
                if ($line -match '^\s+[0-9A-F]{8,16}\s+[0-9A-F]+\s+(\S+)\s*$') { $lines.Add("$tag $dll $($Matches[1])") }
            }
            elseif ($line -match '^\s+[0-9A-F]+\s+(\S+)\s*$' -and $line -notmatch 'Import Address|Name Table|time date|Index of|Characteristics') { $lines.Add("$tag $dll $($Matches[1])") }
        }
    }
    return $lines
}

$failures = 0
foreach ($file in $Files) {
    if (-not (Test-Path -LiteralPath $file)) { Write-Host "SKIP (missing)  $file"; continue }

    $mine = & $harness dump $file
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL (exit $LASTEXITCODE) $file`n  $mine"; $failures++; continue }

    $expected = Get-DumpbinListing $file
    $a = @($mine | ForEach-Object { $_.TrimEnd() } | Sort-Object -CaseSensitive)
    $b = @($expected | ForEach-Object { $_.TrimEnd() } | Sort-Object -CaseSensitive)
    $delta = Compare-Object $a $b -CaseSensitive

    if ($delta) {
        $failures++
        Write-Host "FAIL  $file  (reader $($a.Count), dumpbin $($b.Count), $(@($delta).Count) differ)"
        $delta | Select-Object -First 12 | ForEach-Object {
            $who = if ($_.SideIndicator -eq '<=') { 'reader ' } else { 'dumpbin' }
            Write-Host "    $who  $($_.InputObject)"
        }
    }
    else {
        Write-Host ("OK    {0}  ({1} entries)" -f $file, $a.Count)
    }
}

if ($failures) { throw "$failures file(s) differ from dumpbin." }
Write-Host 'All files match dumpbin.'
