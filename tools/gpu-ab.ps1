# A/B GPU cost: replays a recording once per variant and reports GPU busy
# time (tools\gpu-time.ps1) plus frame-time stats over the same window.
#
#   gpu-ab.ps1 -Variants @{ base = @(); nomsaa = @('--native_2x_msaa=false') }
param(
    [Parameter(Mandatory)][hashtable]$Variants,
    [string]$Recording = 'H:\NIER PORT\recordings\sessao60.csv',
    [string[]]$Common = @('--fps_60=true', '--resolution=1280x720'),
    [double]$From = 35, [double]$Seconds = 40,
    [string]$GameRoot = 'H:\NIER PORT\Nier.iso'
)
$tools = $PSScriptRoot
foreach ($name in $Variants.Keys) {
    $j = Start-Job -FilePath "$tools\gpu-time.ps1" -ArgumentList $From, $Seconds
    $null = & "$tools\replay-measure.ps1" -Recording $Recording -Label "ab-$name" -Seconds ($From + $Seconds + 15) `
        -Every ($From + $Seconds) -GameRoot $GameRoot -Extra ($Common + $Variants[$name])
    $g = Receive-Job $j -Wait -AutoRemoveJob
    while (Get-Process nier -ErrorAction SilentlyContinue) { Start-Sleep -Milliseconds 300 }  # frames.csv still open
    $ft = Import-Csv "H:\NIER PORT\logs\measure-ab-$name\frames.csv" | ForEach-Object { [double]$_.frame_time_us / 1000 }
    # frames.csv has no timestamps: take the frames inside [From, From+Seconds] by cumulative time.
    $t = 0.0; $win = foreach ($x in $ft) { $t += $x; if ($t -ge $From * 1000 -and $t -le ($From + $Seconds) * 1000) { $x } }
    $sorted = $win | Sort-Object; $n = $sorted.Count
    [pscustomobject]@{
        variant = $name; gpu_busy_pct = $g.gpu_busy_pct
        gpu_ms_per_frame = if ($n) { [math]::Round($g.gpu_busy_ms / ($n * $g.window_s / $Seconds), 2) } else { 0 }
        fps = if ($n) { [math]::Round($n / $Seconds, 1) } else { 0 }
        p99_ms = if ($n) { [math]::Round($sorted[[int]($n * 0.99) - 1], 1) } else { 0 }
    }
}
