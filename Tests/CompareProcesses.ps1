<#
.SYNOPSIS
    Checks what ProcScope reports about running processes against PowerShell's own view of them:
    the process list, memory and thread counts, and the modules and threads of a process.
#>
param(
    [ValidateSet('Debug', 'Release')] [string] $Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

$harness = Join-Path $PSScriptRoot "..\bin\x64\$Configuration\ReaderTests.exe"
if (-not (Test-Path $harness)) { throw "ReaderTests.exe not found at $harness - build it first." }
$failures = 0

function Write-Result([bool] $ok, [string] $text) {
    if ($ok) { Write-Host "OK    $text" } else { Write-Host "FAIL  $text"; $script:failures++ }
}

# Process list. Processes come and go while this runs, so a few differences are allowed.
$reference = @{}
Get-Process | ForEach-Object { $reference[[int]$_.Id] = $_ }
$lines = & $harness procs
$rows = @($lines | Where-Object { $_ -match '^\d+\|' } | ForEach-Object {
    $f = $_ -split '\|'
    [pscustomobject]@{ Pid = [int]$f[0]; Name = $f[2]; WorkingSet = [double]$f[4]; Threads = [int]$f[5] }
})
$found = @($rows | Where-Object { $reference.ContainsKey($_.Pid) }).Count
$share = if ($reference.Count) { $found / $reference.Count } else { 0 }
Write-Result ($share -ge 0.9) ("process list: {0} rows, {1} of PowerShell's {2} found" -f $rows.Count, $found, $reference.Count)

$names = @($rows | Where-Object { $reference.ContainsKey($_.Pid) -and $_.Pid -gt 4 -and $reference[$_.Pid].ProcessName -ne ($_.Name -replace '\.exe$', '') })
Write-Result ($names.Count -eq 0) ("process names: {0} differ" -f $names.Count)

$total = ($lines | Where-Object { $_ -match '^total ' })
Write-Result ($total -match 'memoryTotal=[1-9]') "system totals: $total"

# One process we own, so nothing about it is restricted.
$process = Start-Process "$env:WINDIR\System32\cmd.exe" -ArgumentList '/k' -PassThru -WindowStyle Hidden
try {
    Start-Sleep -Seconds 2
    $process.Refresh()

    $row = @($(& $harness procs) | Where-Object { $_ -match "^$($process.Id)\|" })
    Write-Result ($row.Count -eq 1) "started process is listed: $row"
    if ($row.Count -eq 1) {
        $f = $row[0] -split '\|'
        Write-Result ([int]$f[5] -eq $process.Threads.Count) ("thread count: {0} here, {1} in PowerShell" -f $f[5], $process.Threads.Count)
        $ratio = [double]$f[4] / [math]::Max($process.WorkingSet64, 1)
        Write-Result ($ratio -gt 0.5 -and $ratio -lt 2) ("working set: {0:N0}, PowerShell {1:N0}" -f [double]$f[4], $process.WorkingSet64)
    }

    $detail = & $harness modules $process.Id
    $mine = @($detail | Where-Object { $_ -like 'M *' } | ForEach-Object { $_.Substring(2).ToLowerInvariant() } | Sort-Object)
    $theirs = @($process.Modules | ForEach-Object { $_.FileName.ToLowerInvariant() } | Sort-Object)
    $delta = @(Compare-Object $mine $theirs)
    Write-Result ($mine.Count -gt 0 -and $delta.Count -eq 0) ("loaded modules: {0} here, {1} in PowerShell, {2} differ" -f $mine.Count, $theirs.Count, $delta.Count)
    $delta | Select-Object -First 8 | ForEach-Object { Write-Host "    $($_.SideIndicator) $($_.InputObject)" }

    $threads = @($detail | Where-Object { $_ -like 'T *' })
    Write-Result ($threads.Count -eq $process.Threads.Count) ("threads listed: {0}, PowerShell {1}" -f $threads.Count, $process.Threads.Count)
    Write-Result (@($threads | Where-Object { $_ -match '\+0x' }).Count -gt 0) 'thread start addresses map to modules'
}
finally {
    if (-not $process.HasExited) { $process.Kill() }
}

if ($failures) { throw "$failures check(s) failed." }
Write-Host 'All process checks passed.'
