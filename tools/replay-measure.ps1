# Replay a recording with extra runtime flags and measure each screenshot's
# mean brightness (0-255, central 80% of the client area) plus the game's
# input-poll rate from the replay trace (one poll per game logic frame).
#
#   replay-measure.ps1 -Recording recordings\x.csv -Label none -Seconds 90 -Extra '--readback_resolve=none'
# -Toml replaces lines of the recording's nier.toml (settings saved there win
# over command line flags):  -Toml 'draw_resolution_scale_x = 2'
param(
    [Parameter(Mandatory)][string]$Recording,
    [Parameter(Mandatory)][string]$Label,
    [int]$Seconds = 90,
    [int]$Every = 5,
    [string[]]$Extra = @(),
    [string[]]$Toml = @(),
    [string]$Mods = '',  # folder copied to MODS next to nier.toml (texture mods)
    [string]$Root = 'H:\NIER PORT',
    [string]$Exe = (Join-Path $Root 'nier-port\out\build\win-amd64-release\nier.exe'),
    [string]$GameRoot = (Join-Path $Root 'NieR_Extraido')  # folder or .iso
)

Add-Type -AssemblyName System.Drawing
$out = Join-Path $Root "logs\measure-$Label"
Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
$shots = @(); for ($t = $Every; $t -le $Seconds; $t += $Every) { $shots += $t }

$env:NIER_INPUT_REPLAY = (Resolve-Path $Recording).Path
# Isolated settings: the player's nier.toml (resolution, AA...) must not leak
# into measurements, and must not be overwritten by them.
New-Item -ItemType Directory -Force $out | Out-Null
if ($Mods) { Copy-Item $Mods (Join-Path $out 'MODS') -Recurse }
Set-Content -LiteralPath "$out\nier.toml" -Value '# measurement run' -Encoding ascii
# A recording made with record.cmd carries its starting state in <name>.state:
# start from copies of that save data and nier.toml.
$state = [IO.Path]::ChangeExtension((Resolve-Path $Recording).Path, '.state')
if (Test-Path $state) {
    if (Test-Path "$state\nier.toml") {
        Get-Content "$state\nier.toml" | Where-Object { $_ -notmatch '^\s*(game_data_root|fullscreen)\s*=' } |
            Set-Content -LiteralPath "$out\nier.toml" -Encoding utf8
    }
    if (Test-Path "$state\userdata") { Copy-Item "$state\userdata" "$out\userdata" -Recurse }
}
if ($Toml) {
    $names = $Toml | ForEach-Object { ($_ -split '=')[0].Trim() }
    $kept = Get-Content -LiteralPath "$out\nier.toml" |
        Where-Object { $names -notcontains (($_ -split '=')[0].Trim()) }
    @($kept) + $Toml | Set-Content -LiteralPath "$out\nier.toml" -Encoding utf8
}
$env:NIER_CONFIG = "$out\nier.toml"
# perf_overlay: the F2 fps meter on, so whoever is watching sees the numbers.
$result = & (Join-Path $Root 'nier-port\scripts\smoke-run.ps1') `
    -Exe $Exe `
    -GameRoot $GameRoot -OutDir $out -Shots $shots `
    -Extra (@('--headless=true', '--headless_keyboard_text=Nier', '--perf_overlay=true',
              "--perf_log_csv=`"$out\frames.csv`"",
              # Own save data every run (a copy of the recording's starting
              # state, or empty): the player's saves are never touched.
              "--user_data_root=`"$out\userdata`"") + $Extra)
Remove-Item Env:NIER_INPUT_REPLAY
Remove-Item Env:NIER_CONFIG

function Get-Brightness([string]$png) {
    $bmp = [Drawing.Bitmap]::FromFile($png)
    try {
        $top = [int]($bmp.Height * 0.12); $bottom = [int]($bmp.Height * 0.92)
        $left = [int]($bmp.Width * 0.10); $right = [int]($bmp.Width * 0.90)
        $sum = 0.0; $n = 0
        for ($y = $top; $y -lt $bottom; $y += 16) {
            for ($x = $left; $x -lt $right; $x += 16) {
                $c = $bmp.GetPixel($x, $y)
                $sum += 0.299 * $c.R + 0.587 * $c.G + 0.114 * $c.B; $n++
            }
        }
        return [math]::Round($sum / $n, 1)
    } finally { $bmp.Dispose() }
}

$rows = foreach ($p in Get-ChildItem $out -Filter 'shot-*.png' | Sort-Object Name) {
    [pscustomobject]@{ t = [int]($p.BaseName -replace '\D', ''); brightness = Get-Brightness $p.FullName }
}

# Poll rate per 300-poll window from the replay trace.
$trace = "$((Resolve-Path $Recording).Path).replay.csv"
$hb = Get-Content $trace | Where-Object { $_ -match '^H,' } | ForEach-Object {
    $f = $_.Split(','); [pscustomobject]@{ poll = [long]$f[1]; ms = [long]$f[3] } }
$rates = for ($i = 1; $i -lt $hb.Count; $i++) { 300000.0 / ($hb[$i].ms - $hb[$i - 1].ms) }
$fatal = Select-String -LiteralPath (Join-Path $out 'run.log') -Pattern 'FATAL' -ErrorAction SilentlyContinue

[pscustomobject]@{
    label = $Label
    run = $result
    fatal = if ($fatal) { $fatal[0].Line } else { '' }
    polls_per_s_mean = if ($rates) { [math]::Round(($rates | Measure-Object -Average).Average, 1) } else { 0 }
    polls_per_s_min = if ($rates) { [math]::Round(($rates | Measure-Object -Minimum).Minimum, 1) } else { 0 }
    brightness = ($rows | ForEach-Object { "$($_.t)s:$($_.brightness)" }) -join ' '
} | Format-List | Out-String -Width 400
Copy-Item $trace (Join-Path $out 'replay-trace.csv') -ErrorAction SilentlyContinue
