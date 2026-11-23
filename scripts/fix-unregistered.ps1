# Repeatedly run the game; when it dies on "Call to invalid or unregistered
# function at guest address X", declare X in [entrypoint.functions], regenerate,
# rebuild and run again. Stops on any other outcome, a repeated address, or MaxRounds.
param(
    [string]$Root = 'H:\NIER PORT',
    [int]$MaxRounds = 15,
    [int]$RunSeconds = 60
)

$proj = Join-Path $Root 'nier-port'
$manifest = Join-Path $proj 'nier_manifest.toml'
$rexglue = Join-Path $Root 'sdk-src\out\install\win-amd64\bin\rexglue.exe'
$exe = Join-Path $proj 'out\build\win-amd64-release\nier.exe'
$seen = @{}

for ($round = 1; $round -le $MaxRounds; $round++) {
    $out = Join-Path $Root ("logs\fix-{0:yyyyMMdd-HHmmss}-r$round" -f (Get-Date))
    $shots = @(30, [int]($RunSeconds / 2), ($RunSeconds - 5)) | Sort-Object -Unique
    $result = & (Join-Path $PSScriptRoot 'smoke-run.ps1') -Exe $exe -GameRoot (Join-Path $Root 'NieR_Extraido') `
        -OutDir $out -Shots $shots -Extra @("--perf_log_csv=`"$out\frames.csv`"")
    $log = Get-Content (Join-Path $out 'run.log') -Raw -ErrorAction SilentlyContinue
    $m = [regex]::Matches("$log", 'unregistered function at guest address 0x([0-9A-Fa-f]{8})')
    if ($m.Count -eq 0) {
        "round ${round}: $result (no unregistered-function fatal) -> stop"
        break
    }
    $addr = $m[$m.Count - 1].Groups[1].Value.ToUpper()
    if ($seen.ContainsKey($addr) -or (Select-String -LiteralPath $manifest -Pattern "^0x$addr\b" -Quiet)) {
        "round ${round}: 0x$addr again after declaring it -> stop"
        break
    }
    $seen[$addr] = $true
    "round ${round}: $result; unregistered 0x$addr -> declaring"
    Add-Content -LiteralPath $manifest -Encoding utf8 `
        -Value "0x$addr = { name = `"vcall_$addr`" }  # unregistered indirect call target (runtime fatal)"

    Push-Location $proj
    & $rexglue --log-file (Join-Path $out 'codegen.log') codegen nier_manifest.toml *> (Join-Path $out 'codegen-console.log')
    $cg = $LASTEXITCODE
    Pop-Location
    if ($cg -ne 0) { "codegen failed (see $out) -> stop"; break }

    cmd /c "call `"$PSScriptRoot\env.cmd`" && cd /d `"$proj`" && cmake --build --preset win-amd64-release" *> (Join-Path $out 'build.log')
    if ($LASTEXITCODE -ne 0) { "build failed (see $out) -> stop"; break }
}
