# Drop discovered entry points that split a real function: after codegen, every
# "Unresolved ... <T> from <S>" means a declared start lies between S and T.
# Repeats codegen until no Unresolved warnings remain (or nothing left to drop).
param(
    [string]$Project = (Join-Path $PSScriptRoot '..'),
    [Parameter(Mandatory)][string]$Rexglue,
    [int]$MaxPasses = 10
)

$toml = Join-Path $Project 'config\discovered_functions.toml'
for ($pass = 1; $pass -le $MaxPasses; $pass++) {
    Push-Location $Project
    $console = & $Rexglue codegen nier_manifest.toml --ignore-stamp 2>&1 | ForEach-Object { "$_" }
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { "pass ${pass}: codegen failed"; $console | Select-Object -Last 15; exit 1 }

    $spans = foreach ($line in $console) {
        if ($line -match '^Unresolved .*?0x([0-9A-F]{8}) from 0x([0-9A-F]{8})') {
            $t = [Convert]::ToUInt32($Matches[1], 16); $s = [Convert]::ToUInt32($Matches[2], 16)
            , @([Math]::Min($t, $s), [Math]::Max($t, $s))
        }
    }
    if (-not $spans) { "pass ${pass}: 0 unresolved -> done"; exit 0 }

    $lines = Get-Content -LiteralPath $toml
    $cands = $lines | Where-Object { $_ -match '^0x([0-9A-F]{8}) =' } |
        ForEach-Object { [Convert]::ToUInt32($_.Substring(2, 8), 16) } | Sort-Object
    $drop = @{}
    foreach ($sp in $spans) {
        # Rule 1: a declared start between source and target splits a function.
        $inside = $cands | Where-Object { $_ -gt $sp[0] -and $_ -le $sp[1] }
        if ($inside) { $inside | ForEach-Object { $drop['{0:X8}' -f $_] = $true }; continue }
        # Rule 2: the closest start before the branch truncated the function
        # gap-fill had built there, leaving this code with no owner.
        $before = $cands | Where-Object { $_ -le $sp[0] -and $sp[0] - $_ -lt 0x1000 } | Select-Object -Last 1
        if ($before) { $drop['{0:X8}' -f $before] = $true }
    }
    "pass ${pass}: $(@($spans).Count) unresolved, dropping $($drop.Count)"
    if ($drop.Count -eq 0) { "nothing to drop -> stop"; exit 1 }
    $kept = $lines | Where-Object { -not ($_ -match '^0x([0-9A-F]{8}) =' -and $drop.ContainsKey($Matches[1])) }
    Set-Content -LiteralPath $toml -Value $kept -Encoding ascii
}
