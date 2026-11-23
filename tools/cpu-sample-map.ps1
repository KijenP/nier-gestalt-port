# Names the "nier.exe+0x..." entries of a NIER_CPU_SAMPLE report with the
# linker map (configure the game with -DCMAKE_EXE_LINKER_FLAGS_RELEASE=
# -Wl,/MAP:nier.map) and re-sums them per function.
#   cpu-sample-map.ps1 -Report logs\cpu.txt [-Top 40]
param(
    [Parameter(Mandatory)][string]$Report,
    [string]$Map = 'H:\NIER PORT\nier-port\out\build\win-amd64-release\nier.map',
    [int]$Top = 40
)

# Code symbols: " 0001:offset  name  rva+base  object".
$symbols = foreach ($line in [IO.File]::ReadLines($Map)) {
    if ($line -match '^\s*0001:[0-9a-f]+\s+(\S+)\s+([0-9a-f]{16})\s') {
        [pscustomobject]@{ rva = [Convert]::ToUInt64($matches[2], 16) - 0x140000000; name = $matches[1] }
    }
}
$symbols = $symbols | Sort-Object rva
$rvas = [uint64[]]($symbols | ForEach-Object { $_.rva })

function Resolve([uint64]$rva) {
    $i = [Array]::BinarySearch($rvas, $rva)
    if ($i -lt 0) { $i = (-bnot $i) - 1 }
    if ($i -lt 0) { return '?' }
    return $symbols[$i].name
}

$section = ''
$sums = @{ self = @{}; inclusive = @{}; stacks = @{} }
$evaluator = [Text.RegularExpressions.MatchEvaluator] {
    param($m) Resolve ([Convert]::ToUInt64($m.Groups[1].Value, 16))
}
foreach ($line in Get-Content $Report) {
    if ($line -match '^== (\w+)') { $section = $matches[1]; continue }
    if (-not $section -or $line -notmatch '^([\d.,]+)%\t(.+)$') { continue }
    $pct = [double]($matches[1] -replace ',', '.')
    $name = [regex]::Replace($matches[2], 'nier\.exe\+0x([0-9A-F]+)', $evaluator)
    $table = $sums[$section]
    $table[$name] = $pct + $(if ($table.ContainsKey($name)) { $table[$name] } else { 0 })
}
foreach ($s in 'self', 'inclusive', 'stacks') {
    "== $s"
    $sums[$s].GetEnumerator() | Sort-Object Value -Descending | Select-Object -First $Top |
        ForEach-Object { '{0,7:N2}%  {1}' -f $_.Value, $_.Key }
}
