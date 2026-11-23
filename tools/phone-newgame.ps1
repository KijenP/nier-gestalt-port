# On the Android handheld: launches the game, starts a New Game (Normal, name
# "Nier") and waits in the prologue's first 3D scene, then takes screenshots
# (the F2 overlay shows fps / frame time) and closes the game (overheating).
# env.txt must have NIER_INPUT_LIVE=/sdcard/NierPort/pad.txt and nier.toml
# headless = true + headless_keyboard_text = "Nier".
#   phone-newgame.ps1 -Label base
param(
    [Parameter(Mandatory)][string]$Label,
    [int]$Settle = 30,           # seconds in the scene before the first shot
    [int]$Shots = 3,
    [int]$Every = 10,
    [switch]$KeepRunning,
    [switch]$NoLaunch,           # the game was started by someone else (Streamline)
    [string]$Adb = 'H:\Android\sdk\platform-tools\adb.exe'
)
$phone = Join-Path $PSScriptRoot 'phone.ps1'
if ($NoLaunch) { Start-Sleep 25 } else { & $phone -Start -Wait 25 | Out-Null }
& $phone -Pad START -Wait 4 | Out-Null          # intro video -> Press START
& $phone -Pad START -Wait 5 | Out-Null          # -> "System data loaded."
& $phone -Pad A -Wait 3 | Out-Null              # -> title menu (New Game)
& $phone -Pad A -Wait 3 | Out-Null              # -> difficulty (Normal)
& $phone -Pad A -Wait 4 | Out-Null              # -> overwrite system data?
& $phone -Pad LEFT,A -Wait 6 | Out-Null         # Yes -> "System data saved."
& $phone -Pad A -Wait 10 | Out-Null             # -> "Set your name as Nier?" (no keyboard; slow)
# LEFT and A together are sometimes taken as A alone (on No), so apart; the
# LEFT is sometimes lost too: while the cursor blob is still on "No" (bright
# pixels around 712,413 of the 1280x720 shot), try again.
Add-Type -AssemblyName System.Drawing
for ($try = 0; $try -lt 4; $try++) {
    & $phone -Pad LEFT -Wait 2 | Out-Null       # Yes
    & $phone -Pad A -Wait 3 -Shot "$Label-prompt" | Out-Null
    $img = [Drawing.Bitmap]::FromFile((Join-Path 'H:\NIER PORT\logs\phone' "$Label-prompt.png"))
    $onNo = $img.GetPixel(700, 413).GetBrightness() -gt 0.3 -and $img.GetPixel(584, 300).GetBrightness() -lt 0.1
    $img.Dispose()
    if (-not $onNo) { break }
}
Start-Sleep ([math]::Max(0, $Settle - 3))       # -> prologue
for ($i = 1; $i -le $Shots; $i++) {
    & $phone -Wait 0 -Shot "$Label-$i"
    if ($i -lt $Shots) { Start-Sleep $Every }
}
if (-not $KeepRunning) { & $Adb shell "am force-stop com.nierport.gestalt" }
