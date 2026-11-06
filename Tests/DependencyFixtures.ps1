<#
.SYNOPSIS
    Builds small test DLLs with known faults, and checks that the dependency check reports each
    fault, with the right severity, and reports nothing on a healthy chain.

.DESCRIPTION
    Uses main.dll, which imports functions from leaf.dll:
      clean        leaf.dll present                          no errors, no warnings
      missing      leaf.dll deleted                          error: not found
      function     leaf.dll rebuilt without the function     error: function missing
      wrongarch    leaf.dll rebuilt as 32-bit                error: wrong architecture
      corrupt      leaf.dll is not a PE file                 error: can't be read
      delay        delay-loaded, leaf.dll deleted            warning, no error
      forward      leaf.dll forwards to a DLL that's absent  error: not found, through the forward
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$harness = Join-Path $PSScriptRoot "..\bin\x64\$Configuration\ReaderTests.exe"
if (-not (Test-Path $harness)) { throw "ReaderTests.exe not found at $harness - build it first." }
$harness = (Resolve-Path $harness).Path

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -version '[17,18)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
$vcvars = "$vs\VC\Auxiliary\Build\vcvarsall.bat"

$root = Join-Path ([IO.Path]::GetTempPath()) ("LibScopeFixtures_" + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null

function Invoke-Build([string] $arch, [string] $folder, [string[]] $commands) {
    $script = Join-Path $folder 'build.cmd'
    @('@echo off', "call `"$vcvars`" $arch >nul") + ($commands | ForEach-Object { "$_ || exit /b 1" }) | Set-Content -Path $script -Encoding Ascii
    Push-Location $folder
    try {
        $output = & cmd.exe /c '.\build.cmd 2>&1'
        if ($LASTEXITCODE -ne 0) { throw "Fixture build failed in $folder`:`n$($output -join "`n")" }
    }
    finally { Pop-Location }
}

function New-Fixture([string] $name, [string] $mainFlags = '') {
    $folder = Join-Path $root $name
    New-Item -ItemType Directory -Path $folder | Out-Null
    Set-Content -Path "$folder\leaf.cpp" -Encoding Ascii -Value 'extern "C" __declspec(dllexport) int LeafOne() { return 1; }
extern "C" __declspec(dllexport) int LeafTwo() { return 2; }'
    Set-Content -Path "$folder\main.cpp" -Encoding Ascii -Value 'extern "C" __declspec(dllimport) int LeafOne();
extern "C" __declspec(dllimport) int LeafTwo();
extern "C" __declspec(dllexport) int MainFunction() { return LeafOne() + LeafTwo(); }'
    Invoke-Build 'x64' $folder @(
        'cl /nologo /LD /MT leaf.cpp /Fe:leaf.dll',
        "cl /nologo /LD /MT main.cpp leaf.lib $mainFlags /Fe:main.dll"
    )
    return $folder
}

function Remove-Leaf([string] $folder) { Remove-Item -LiteralPath "$folder\leaf.dll" }

# Each case: what the output has to contain, on top of the exact error and warning counts.
function Test-Case([string] $name, [string] $folder, [int] $errors, [int] $warnings, [string[]] $expect) {
    $lines = & $harness deps "$folder\main.dll"
    $summary = $lines | Where-Object { $_ -match '^errors=' } | Select-Object -Last 1
    $failed = @()
    # The chain includes Windows itself, whose optional DLLs always add warnings. Only the fixture's own count here.
    $own = @($lines | Where-Object { $_ -match '^WARNING' -and $_ -match '\b(main|leaf|absent)\.dll' }).Count
    if ($summary -notmatch "^errors=$errors ") { $failed += "expected errors=$errors, got: $summary" }
    if ($own -ne $warnings) { $failed += "expected $warnings warnings about the fixture's DLLs, got $own" }
    foreach ($text in $expect) {
        if (-not ($lines | Where-Object { $_ -like "*$text*" })) { $failed += "missing output: $text" }
    }
    if ($failed) {
        Write-Host "FAIL  $name"
        $failed | ForEach-Object { Write-Host "    $_" }
        $lines | Where-Object { $_ -match '^(ERROR|WARNING|INFO|errors=)' } | Select-Object -First 12 | ForEach-Object { Write-Host "    | $_" }
        $script:failures++
    }
    else { Write-Host "OK    $name  ($summary)" }
}

$script:failures = 0
try {
    $f = New-Fixture 'clean'
    Test-Case 'healthy chain' $f 0 0 @('MODULE leaf.dll|0|required|')

    $f = New-Fixture 'missing'
    Remove-Leaf $f
    Test-Case 'missing DLL' $f 1 0 @('ERROR main.dll needs leaf.dll, which was not found')

    $f = New-Fixture 'function'
    Set-Content -Path "$f\leaf.cpp" -Encoding Ascii -Value 'extern "C" __declspec(dllexport) int LeafTwo() { return 2; }'
    Invoke-Build 'x64' $f @('cl /nologo /LD /MT leaf.cpp /Fe:leaf.dll')
    Test-Case 'missing function' $f 1 0 @("ERROR main.dll needs 1 function that leaf.dll doesn't export: LeafOne")

    $f = New-Fixture 'wrongarch'
    Invoke-Build 'x86' $f @('cl /nologo /LD /MT leaf.cpp /Fe:leaf.dll')
    Test-Case 'wrong architecture' $f 1 0 @('ERROR main.dll needs leaf.dll: This is a x86 file, but the chain needs x64')

    $f = New-Fixture 'corrupt'
    Set-Content -Path "$f\leaf.dll" -Encoding Ascii -Value 'this is not a PE file'
    Test-Case 'unreadable DLL' $f 1 0 @("ERROR main.dll needs leaf.dll, which can't be read")

    $f = New-Fixture 'delay' '/link /DELAYLOAD:leaf.dll delayimp.lib'
    Remove-Leaf $f
    Test-Case 'missing delay-loaded DLL' $f 0 1 @('WARNING main.dll delay-loads leaf.dll, which was not found')

    $f = New-Fixture 'forward'
    Set-Content -Path "$f\leaf.cpp" -Encoding Ascii -Value 'extern "C" __declspec(dllexport) int LeafTwo() { return 2; }
#pragma comment(linker, "/export:LeafOne=absent.Somewhere")'
    Invoke-Build 'x64' $f @('cl /nologo /LD /MT leaf.cpp /Fe:leaf.dll')
    Test-Case 'forward to a missing DLL' $f 1 0 @('ERROR leaf.dll forwards to absent.dll, which was not found')
}
finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}

if ($script:failures) { throw "$script:failures fixture case(s) failed." }
Write-Host 'All fixture cases passed.'
