# Summarize a --perf_log_csv file: frame-time percentiles overall and per
# time window, so stutter shows up instead of hiding in an average.
#
#   frame-stats.ps1 -Csv logs\measure-x\frames.csv [-Window 5] [-From 35] [-To 60]
param(
    [Parameter(Mandatory)][string]$Csv,
    [double]$Window = 5,
    [double]$From = 0,
    [double]$To = [double]::MaxValue
)

$ft = Import-Csv $Csv | ForEach-Object { [double]$_.frame_time_us / 1000.0 }
$t = 0.0
$frames = foreach ($ms in $ft) { $t += $ms / 1000.0; [pscustomobject]@{ t = $t; ms = $ms } }
$sel = @($frames | Where-Object { $_.t -ge $From -and $_.t -le $To })

function Pct([double[]]$v, [double]$p) {
    $s = $v | Sort-Object; $s[[math]::Min($s.Count - 1, [int][math]::Floor($p / 100 * $s.Count))]
}
function Summ($set) {
    $v = [double[]]($set | ForEach-Object { $_.ms })
    $dur = ($v | Measure-Object -Sum).Sum / 1000
    [pscustomobject]@{
        frames = $v.Count
        fps = [math]::Round($v.Count / [math]::Max($dur, 0.001), 1)
        p50_ms = [math]::Round((Pct $v 50), 1)
        p95_ms = [math]::Round((Pct $v 95), 1)
        p99_ms = [math]::Round((Pct $v 99), 1)
        max_ms = [math]::Round(($v | Measure-Object -Maximum).Maximum, 1)
        over_20ms = ($v | Where-Object { $_ -gt 20 }).Count
        over_40ms = ($v | Where-Object { $_ -gt 40 }).Count
    }
}

"== total {0:N1}s-{1:N1}s" -f $sel[0].t, $sel[-1].t
Summ $sel | Format-Table -AutoSize | Out-String -Width 200
"== por janela de $Window s"
$sel | Group-Object { [math]::Floor($_.t / $Window) * $Window } | ForEach-Object {
    $s = Summ $_.Group
    [pscustomobject]@{ from_s = [double]$_.Name; fps = $s.fps; p50 = $s.p50_ms; p99 = $s.p99_ms; max = $s.max_ms; over20 = $s.over_20ms }
} | Format-Table -AutoSize | Out-String -Width 200
