<#
.SYNOPSIS
  Soak test without an iPhone (R13): the fake phone streams the test clip to the headless engine for
  a long time while this samples the engine's memory, handles and threads. A leak shows up as a
  steady climb; a stall as missing frames in the engine's own per-second stats.

  Writes build\soak-<stamp>.csv (samples) and build\soak-<stamp>-engine.log (engine output).

.EXAMPLE
  .\windows\scripts\soak.ps1 -Minutes 60
  .\windows\scripts\soak.ps1 -Minutes 10 -Camera   # also feed Cabled Camera (quit Cabled first)
#>
param(
    [double]$Minutes = 30,
    [int]$SampleSeconds = 30,
    [switch]$Camera
)

$ErrorActionPreference = 'Stop'
$repo = Resolve-Path (Join-Path $PSScriptRoot '..\..')
$bin = Join-Path $repo 'build\bin\Release'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$csv = Join-Path $repo "build\soak-$stamp.csv"
$engineLog = Join-Path $repo "build\soak-$stamp-engine.log"
$seconds = [int]($Minutes * 60)

$phone = Start-Process -PassThru -WindowStyle Hidden (Join-Path $bin 'cabled-fakephone.exe') `
    -ArgumentList "--clip `"$(Join-Path $repo 'windows\testdata\pattern_1080p30.h264')`""
Start-Sleep -Seconds 1
$engineArgs = "--tcp 127.0.0.1:47800 --seconds $seconds" + $(if ($Camera) { '' } else { ' --no-vcam' })
$engine = Start-Process -PassThru -WindowStyle Hidden (Join-Path $bin 'cabled-engine.exe') -ArgumentList $engineArgs `
    -RedirectStandardOutput $engineLog -RedirectStandardError "$engineLog.err"

"elapsed_s,working_set_mb,private_mb,handles,threads,cpu_s" | Set-Content $csv
$start = Get-Date
try {
    while (-not $engine.HasExited) {
        $p = Get-Process -Id $engine.Id -ErrorAction SilentlyContinue
        if (-not $p) { break }
        $line = '{0:0},{1:0.0},{2:0.0},{3},{4},{5:0.0}' -f ((Get-Date) - $start).TotalSeconds, ($p.WorkingSet64 / 1MB),
            ($p.PrivateMemorySize64 / 1MB), $p.HandleCount, $p.Threads.Count, $p.TotalProcessorTime.TotalSeconds
        Add-Content $csv $line
        Write-Host $line
        Start-Sleep -Seconds $SampleSeconds
    }
}
finally {
    if (-not $engine.HasExited) { Stop-Process -Id $engine.Id -Force }
    if (-not $phone.HasExited) { Stop-Process -Id $phone.Id -Force }
}

$rows = Import-Csv $csv
if ($rows.Count -ge 4) {
    # Compare the second quarter with the last: warm-up allocations are excluded.
    $q = [int]($rows.Count / 4)
    $early = ($rows[$q..(2 * $q - 1)] | Measure-Object private_mb -Average).Average
    $late = ($rows[(3 * $q)..($rows.Count - 1)] | Measure-Object private_mb -Average).Average
    $handlesEarly = [int]$rows[$q].handles
    $handlesLate = [int]$rows[-1].handles
    Write-Host ("Private memory: {0:0.0} MB -> {1:0.0} MB ({2:+0.0;-0.0} MB). Handles: {3} -> {4}." -f $early, $late,
        ($late - $early), $handlesEarly, $handlesLate)
}
# The engine's own per-second lines: frame rate, keyframe requests (drops), reconnects.
$stats = Select-String -Path $engineLog -Pattern '^\[streaming\].*?(\d+\.\d) fps.*kf-req (\d+), reconnects (\d+)' |
    ForEach-Object { [pscustomobject]@{ Fps = [double]$_.Matches[0].Groups[1].Value
                                       Kf = [int]$_.Matches[0].Groups[2].Value
                                       Reconnects = [int]$_.Matches[0].Groups[3].Value } }
if ($stats) {
    $steady = $stats | Select-Object -Skip 5  # skip start-up
    $fps = $steady | Measure-Object Fps -Minimum -Average
    Write-Host ("Streaming {0} s: fps avg {1:0.0}, min {2:0.0}; keyframe requests {3}; reconnects {4}." -f $stats.Count,
        $fps.Average, $fps.Minimum, $stats[-1].Kf, $stats[-1].Reconnects)
} else {
    Write-Host 'WARNING: the engine never reported streaming. Nothing was decoded.'
}
Write-Host "Samples: $csv"
Write-Host "Engine output: $engineLog"
