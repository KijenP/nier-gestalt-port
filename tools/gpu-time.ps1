# GPU busy time of nier.exe from Windows' "GPU Engine\Running Time" counter
# (cumulative, 100 ns units, per process and engine). Waits for a newly
# started game, skips -From seconds, then reports 3D-engine busy time over
# -Seconds. Run it as a background job alongside replay-measure.ps1:
#
#   $j = Start-Job -FilePath tools\gpu-time.ps1 -ArgumentList 35, 40
#   replay-measure.ps1 ...; Receive-Job $j -Wait
param([double]$From = 35, [double]$Seconds = 40)

$deadline = (Get-Date).AddMinutes(3)
$launched_after = (Get-Date).AddSeconds(-2)  # not the previous run's closing process
while (-not ($p = Get-Process nier -ErrorAction SilentlyContinue |
                 Where-Object { $_.StartTime -gt $launched_after } | Select-Object -First 1)) {
    if ((Get-Date) -gt $deadline) { return [pscustomobject]@{ error = 'nier.exe not found' } }
    Start-Sleep -Milliseconds 200
}
$start = $p.StartTime
while (((Get-Date) - $start).TotalSeconds -lt $From) { Start-Sleep -Milliseconds 100 }

# Sample every 2 s and keep the first and last good reading: a single read
# occasionally comes back empty, which made two-read versions report nothing.
$path = "\GPU Engine(pid_$($p.Id)_*engtype_3D)\Running Time"
$first = $null; $last = $null
$end = $start.AddSeconds($From + $Seconds)
while ((Get-Date) -lt $end) {
    $s = (Get-Counter $path -ErrorAction SilentlyContinue).CounterSamples | Select-Object -First 1
    if ($s -and $s.RawValue -gt 0) {
        $r = [pscustomobject]@{ t = Get-Date; v = [double]$s.RawValue; name = $s.InstanceName }
        if (-not $first) { $first = $r } elseif ($r.name -eq $first.name) { $last = $r }
    }
    Start-Sleep -Seconds 2
}
if (-not $last) { return [pscustomobject]@{ error = 'counter samples missing' } }
$busy = ($last.v - $first.v) / 1e4  # 100 ns -> ms
$wall = ($last.t - $first.t).TotalMilliseconds
[pscustomobject]@{
    from_s = $From; window_s = [math]::Round($wall / 1000, 1)
    gpu_busy_ms = [math]::Round($busy); gpu_busy_pct = [math]::Round(100 * $busy / $wall, 1)
}
