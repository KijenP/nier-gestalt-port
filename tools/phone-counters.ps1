# Mali GPU counters on the Android handheld (Arm Streamline, gatord) over the
# prologue's first 3D scene: launches the game under a headless capture,
# drives it there (phone-newgame.ps1), sums the counters over a window and
# closes the game. Prints the counter sums of the window.
#   phone-counters.ps1 -Label base
param(
    [Parameter(Mandatory)][string]$Label,
    [double]$From = 105, [double]$To = 145,       # capture seconds summed (the scene)
    [string]$Studio = 'H:\Tools\ArmPerformanceStudio\2026.5\PFiles64\Arm\Arm Performance Studio 2026.5',
    [string]$Out = 'H:\NIER PORT\logs\streamline',
    [string]$Adb = 'H:\Android\sdk\platform-tools\adb.exe'
)
$sl = Join-Path $Studio 'streamline'
$apc = Join-Path $Out "$Label.apc"
$env:PATH = "$(Split-Path $Adb);$env:PATH"
& $Adb shell "am force-stop com.nierport.gestalt; echo 0 > /sdcard/NierPort/pad.txt; rm -f /sdcard/NierPort/log.txt" | Out-Null
$p = Start-Process python -PassThru -NoNewWindow -RedirectStandardOutput "$Out\$Label.out" -RedirectStandardError "$Out\$Label.err" -ArgumentList @(
    '-I', "`"$sl\bin\android\streamline_me.py`"", '--package', 'com.nierport.gestalt',
    '--package-activity', '.NierActivity', '--headless', "`"$apc`"", '--headless-timeout', '150',
    '--config', "`"$Out\mali-g57.xml`"", '--daemon', "`"$sl\bin\android\arm64\gatord`"",
    '--overwrite', '--gpu-timeline', 'no')
Start-Sleep 15
& (Join-Path $PSScriptRoot 'phone-newgame.ps1') -Label $Label -NoLaunch -KeepRunning -Settle 20 -Shots 4 -Every 8 | Out-Null
$p.WaitForExit()
& $Adb shell "am force-stop com.nierport.gestalt" | Out-Null

$lines = & (Join-Path $sl 'Streamline-cli.exe') -report $apc -timeline 2>$null
$hdr = $lines[6] -split ','
$sum = @{}; $ms = 0
foreach ($l in $lines[7..($lines.Count - 1)]) {
    if ($l -notmatch '^"(\d+),(\d+)",(.*)$') { continue }
    $t = [double]"$($Matches[1]).$($Matches[2])"
    if ($t -lt $From -or $t -ge $To) { continue }
    $v = $Matches[3] -split ','
    for ($k = 0; $k -lt $v.Count; $k++) { $sum[$hdr[$k + 1]] += [double]$v[$k] }
    $ms++
}
# Frame times: the overlay on logs\phone\<Label>-1..4.png.
"window: $ms ms"
$sum.GetEnumerator() | Sort-Object Name | ForEach-Object { '{0,-62} {1,18:N0}' -f $_.Name, $_.Value }
