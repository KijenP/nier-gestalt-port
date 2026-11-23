# Builds the PC and Android versions and packs a release (2026-11-23).
#   <Out>\NieR-Gestalt-PC-<ver>.zip         the game for Windows: EXE (no disc icon), DLLs, tools, PT-BR translation
#   <Out>\Projeto-Gestalt-Android-<ver>.apk the Android installer (android\build-apk.ps1)
#   <Out>\MOD-Trainer-<ver>.zip             MODS\Trainer, off and at its defaults
#   <Out>\MOD-Corpo-Kaine-<ver>.zip         MODS\Corpo-kaine010 (the player's call: a separate, optional download)
# Nothing personal goes in: nier.toml, logs, saves, cache, captures, Music, the disc's icon, test mods.
# Usage: scripts\make-release.ps1 [-Version 1.0] [-SkipBuild]
param([string]$Version = (Get-Date -Format 'yyyy-MM-dd'), [switch]$SkipBuild,
      [string]$Out = "H:\NIER PORT\pacote\release-$((Get-Date -Format 'yyyy-MM-dd'))")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pc = Join-Path $root 'out\build\win-renut'
$android = Join-Path $root 'out\build\android-arm64-h'

if (-not $SkipBuild) {
  # Native tools write progress to stderr: PowerShell 5.1 would stop on it, so only their exit codes count here.
  $ErrorActionPreference = 'Continue'
  cmd /c "call `"$root\scripts\env.cmd`" && cmake --build `"$pc`" 2>&1"
  if ($LASTEXITCODE) { throw 'PC build failed' }
  cmd /c "call `"$root\scripts\env.cmd`" && cmake --build `"$android`" 2>&1"
  if ($LASTEXITCODE) { throw 'Android build failed' }
  & "$root\android\build-apk.ps1" -Build $android 2>&1 | Out-Host
  if (-not (Test-Path "$root\out\project-gestalt.apk") -or
      (Get-Item "$root\out\project-gestalt.apk").LastWriteTime -lt (Get-Date).AddMinutes(-30)) { throw 'APK failed' }
  $ErrorActionPreference = 'Stop'
}

if (Test-Path $Out) { throw "$Out already exists" }
New-Item -ItemType Directory $Out | Out-Null
$stage = Join-Path $Out 'stage'

# --- PC ---
$game = Join-Path $stage 'NieR Gestalt'
New-Item -ItemType Directory $game | Out-Null
Copy-Item "$pc\NieR Gestalt.exe", "$pc\rexruntime.dll", "$pc\NierTextureTool.exe", "$pc\NierTextureTool-cli.exe",
          "$pc\NierTradutor.exe" $game
& "$root\scripts\remove-exe-icon.ps1" -Exe "$game\NieR Gestalt.exe"
foreach ($f in 'msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll') {
  Copy-Item "$env:SystemRoot\System32\$f" $game
}
# The translation, without working copies (.bak, .pre-fix, .teste).
$text = Join-Path $game 'TEXT\Portugues'
Copy-Item "$pc\TEXT\Portugues" $text -Recurse
Get-ChildItem $text -Recurse -File | Where-Object { $_.Extension -in '.bak', '.pre-fix', '.teste' } | Remove-Item

# --- Mods ---
$trainer = Join-Path $stage 'MOD-Trainer\MODS\Trainer'
New-Item -ItemType Directory $trainer | Out-Null
Copy-Item "$pc\MODS\Trainer\trainer.dll" $trainer
$defaults = @('# Nier Plus: this mod''s settings (written by the game)', '_enabled=0', 'damage_times=1', 'exp_times=1',
              'free_cam=0', 'free_cam_speed=8', 'hit_kill=0', 'infinite_hp=0', 'infinite_jump=0', 'infinite_mp=0',
              'invincible=0')
[IO.File]::WriteAllLines("$trainer\settings.txt", $defaults, (New-Object Text.UTF8Encoding $false))

$kaine = Join-Path $stage 'MOD-Corpo-Kaine\MODS\Corpo-kaine010'
New-Item -ItemType Directory $kaine | Out-Null
Copy-Item "$pc\MODS\.Corpo-kaine010\*" $kaine -Recurse -Force
# Working files of the paused Kaine-powers experiment and old copies stay out.
Get-ChildItem $kaine -Recurse -File -Force | Where-Object { $_.Name -in 'poderes.txt' -or $_.Extension -eq '.antes' } |
  Remove-Item -Force

# --- Check: nothing personal or straight from the disc in the game's package ---
$bad = Get-ChildItem $game -Recurse -File -Force | Where-Object {
  $_.Extension -in '.iso', '.cpk', '.sfd', '.xex', '.ico', '.pdb', '.log', '.toml', '.dat', '.mp3', '.bin' -or
  $_.Name -match '(?i)^(nier\.toml|achievements|jukebox|touch_controls|SAVE)'
}
if ($bad) { $bad | ForEach-Object { Write-Warning "not for release: $($_.FullName)" }; throw 'package check failed' }

# --- Zips ---
Compress-Archive -Path "$game" -DestinationPath "$Out\NieR-Gestalt-PC-$Version.zip"
Compress-Archive -Path "$stage\MOD-Trainer\MODS" -DestinationPath "$Out\MOD-Trainer-$Version.zip"
Compress-Archive -Path "$stage\MOD-Corpo-Kaine\MODS" -DestinationPath "$Out\MOD-Corpo-Kaine-$Version.zip"
Copy-Item "$root\out\project-gestalt.apk" "$Out\Projeto-Gestalt-Android-$Version.apk"
Remove-Item $stage -Recurse -Force
Get-ChildItem $Out | Select-Object Name, @{ n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } }
