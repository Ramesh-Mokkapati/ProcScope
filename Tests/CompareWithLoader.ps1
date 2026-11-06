<#
.SYNOPSIS
    Checks the dependency chain against the real Windows loader, on running processes.

.DESCRIPTION
    A process that runs proves two things: every DLL the chain calls required must be loaded from
    exactly that path, and the chain can't report any errors. Runs 64-bit and 32-bit programs from
    Windows, then closes the ones it started.
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$harness64 = Join-Path $PSScriptRoot "..\bin\x64\$Configuration\ReaderTests.exe"
$harness32 = Join-Path $PSScriptRoot "..\bin\Win32\$Configuration\ReaderTests.exe"
foreach ($h in $harness64, $harness32) { if (-not (Test-Path $h)) { throw "$h not found - build $Configuration first." } }

$failures = 0

function Test-Process([string] $harness, [int] $processId) {
    $output = if ($processId) { & $harness loadercheck $processId } else { & $harness loadercheck }
    $code = $LASTEXITCODE
    $output | ForEach-Object { Write-Host $_ }
    if ($code -ne 0) { $script:failures++ }
}

Test-Process $harness64 0
Test-Process $harness32 0

$explorer = Get-Process explorer -ErrorAction SilentlyContinue | Select-Object -First 1
if ($explorer) { Test-Process $harness64 $explorer.Id }

$cases = @(
    @($harness64, "$env:WINDIR\System32\cmd.exe"),
    @($harness64, "$env:WINDIR\System32\charmap.exe"),
    @($harness32, "$env:WINDIR\SysWOW64\cmd.exe"),
    @($harness32, "$env:WINDIR\SysWOW64\charmap.exe")
)
foreach ($case in $cases) {
    # Not every Windows edition has these, build servers especially.
    if (-not (Test-Path -LiteralPath $case[1])) { Write-Host "SKIP (missing)  $($case[1])"; continue }
    $process = Start-Process $case[1] -PassThru -WindowStyle Minimized
    try {
        Start-Sleep -Seconds 2
        Test-Process $case[0] $process.Id
    }
    finally {
        if (-not $process.HasExited) { $process.Kill() }
    }
}

if ($failures) { throw "$failures process(es) differ from the loader." }
Write-Host 'All processes match the loader.'
