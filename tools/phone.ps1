# Drives the game on the Android handheld over adb, for tests with nobody at
# the device: pad presses through the game's NIER_INPUT_LIVE file (env.txt must
# have NIER_INPUT_LIVE=/sdcard/NierPort/pad.txt), then a screenshot (the F2
# overlay on it shows fps / frame time).
#   phone.ps1 -Start
#   phone.ps1 -Pad START,A,DOWN,A -Wait 3 -Shot s4
#   phone.ps1 -Hold UP -For 5            (hold a button, e.g. walk)
param(
    [string[]]$Pad = @(),
    [string]$Hold = '',
    [double]$For = 1,
    [double]$Gap = 0.8,          # seconds between presses
    [double]$Press = 0.35,       # seconds a press is held
    [double]$Wait = 2,           # seconds before the screenshot
    [string]$Shot = '',          # name of the screenshot (logs\phone\<name>.png)
    [switch]$Start,
    [string]$Adb = 'H:\Android\sdk\platform-tools\adb.exe',
    [string]$Out = 'H:\NIER PORT\logs\phone',
    [string]$File = '/sdcard/NierPort/pad.txt'
)
$bits = @{ UP = 0x0001; DOWN = 0x0002; LEFT = 0x0004; RIGHT = 0x0008; START = 0x0010; BACK = 0x0020
           LS = 0x0040; RS = 0x0080; LB = 0x0100; RB = 0x0200; A = 0x1000; B = 0x2000; X = 0x4000; Y = 0x8000 }
New-Item -ItemType Directory -Force $Out | Out-Null
if ($Start) {
    & $Adb shell "echo 0 > $File; am force-stop com.nierport.gestalt; rm -f /sdcard/NierPort/log.txt; am start -n com.nierport.gestalt/.NierActivity" | Out-Null
}
foreach ($p in $Pad) {
    $hex = '{0:X}' -f $bits[$p.ToUpper()]
    & $Adb shell "echo $hex > $File; sleep $Press; echo 0 > $File"
    Start-Sleep -Milliseconds ([int]($Gap * 1000))
}
if ($Hold) {
    $hex = '{0:X}' -f $bits[$Hold.ToUpper()]
    & $Adb shell "echo $hex > $File; sleep $For; echo 0 > $File"
}
Start-Sleep -Milliseconds ([int]($Wait * 1000))
if ($Shot) {
    & $Adb shell "screencap -p /sdcard/shot.png"
    & $Adb pull /sdcard/shot.png (Join-Path $Out "$Shot.png") 2>&1 | Out-Null
    Join-Path $Out "$Shot.png"
}
