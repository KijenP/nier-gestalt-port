# Sums a Streamline capture's Mali counters over a window that starts a fixed
# time after the prologue's loading (the last dip of GPU activity), so runs
# whose menus took different times compare the same part of the scene.
#   counters-sum.ps1 -Apc logs\streamline\base.apc [-After 10] [-Length 30]
param(
    [Parameter(Mandatory)][string]$Apc,
    [double]$After = 10, [double]$Length = 30,
    [double]$From = -1,  # fixed window start (seconds), instead of after the loading

    [string]$Cli = 'H:\Tools\ArmPerformanceStudio\2026.5\PFiles64\Arm\Arm Performance Studio 2026.5\streamline\Streamline-cli.exe'
)
$lines = & $Cli -report $Apc -timeline 2>$null
$hdr = $lines[6] -split ','
$active = [array]::IndexOf($hdr, 'Mali GPU Cycles:GPU active') - 1
$rows = foreach ($l in $lines[7..($lines.Count - 1)]) {
    if ($l -match '^"(\d+),(\d+)",(.*)$') { , @([double]"$($Matches[1]).$($Matches[2])", [double[]]($Matches[3] -split ',')) }
}
# GPU activity per second; loading = a second below 60% of the busiest one.
$perSec = @{}
foreach ($r in $rows) { $perSec[[int][math]::Floor($r[0])] += $r[1][$active] }
$max = ($perSec.Values | Measure-Object -Maximum).Maximum
$secs = $perSec.Keys | Sort-Object
$lastDip = ($secs | Where-Object { $_ -gt 60 -and $_ -lt ($secs[-1] - 5) -and $perSec[$_] -lt 0.6 * $max } | Select-Object -Last 1)
$from = if ($From -ge 0) { $From } else { $lastDip + 1 + $After }; $to = $from + $Length
$sum = New-Object double[] ($hdr.Count - 1)
foreach ($r in $rows) { if ($r[0] -ge $from -and $r[0] -lt $to) { for ($k = 0; $k -lt $r[1].Count; $k++) { $sum[$k] += $r[1][$k] } } }
"loading ends at {0} s; window {1}-{2} s" -f ($lastDip + 1), $from, $to
for ($k = 0; $k -lt $sum.Count; $k++) { '{0,-62} {1,18:N0}' -f $hdr[$k + 1], $sum[$k] }
