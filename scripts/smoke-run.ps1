# Launch the game windowed, capture screenshots at fixed times, then close it.
# Usage: smoke-run.ps1 -Exe <nier.exe> -GameRoot <dir> -OutDir <dir> [-Shots 20,45,80] [-Extra '--flag=x']
param(
    [Parameter(Mandatory)][string]$Exe,
    [Parameter(Mandatory)][string]$GameRoot,
    [Parameter(Mandatory)][string]$OutDir,
    [int[]]$Shots = @(20, 45, 80),
    [string[]]$Extra = @()
)

Add-Type -AssemblyName System.Windows.Forms, System.Drawing
Add-Type -Namespace Smoke -Name Win -MemberDefinition @'
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
[DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
[DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
public struct RECT { public int Left, Top, Right, Bottom; }
'@
# Without this, window rects and screen captures disagree under display scaling.
[Smoke.Win]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force $OutDir | Out-Null

$log = Join-Path $OutDir 'run.log'
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue  # the runtime appends to an existing log
$gameArgs = @("--game_data_root=`"$GameRoot`"", '--fullscreen=false', "--log_file=`"$log`"") + $Extra
$proc = Start-Process -FilePath $Exe -ArgumentList $gameArgs -WorkingDirectory (Split-Path $Exe) -PassThru

$clock = [Diagnostics.Stopwatch]::StartNew()
foreach ($t in $Shots) {
    while ($clock.Elapsed.TotalSeconds -lt $t -and -not $proc.HasExited) { Start-Sleep -Milliseconds 250 }
    if ($proc.HasExited) { break }
    # Capture just the game window, moved fully on screen and to the front.
    $proc.Refresh()
    $h = $proc.MainWindowHandle
    if ($h -ne [IntPtr]::Zero) {
        [Smoke.Win]::SetWindowPos($h, [IntPtr]::new(-1), 0, 0, 0, 0, 0x0001) | Out-Null  # HWND_TOPMOST, NOSIZE
        [Smoke.Win]::SetForegroundWindow($h) | Out-Null
        Start-Sleep -Milliseconds 300
        $r = New-Object Smoke.Win+RECT
        [Smoke.Win]::GetWindowRect($h, [ref]$r) | Out-Null
        $b = [Drawing.Rectangle]::FromLTRB($r.Left, $r.Top, $r.Right, $r.Bottom)
    } else {
        $b = [Windows.Forms.Screen]::PrimaryScreen.Bounds
    }
    $bmp = New-Object Drawing.Bitmap $b.Width, $b.Height
    [Drawing.Graphics]::FromImage($bmp).CopyFromScreen($b.Location, [Drawing.Point]::Empty, $b.Size)
    $bmp.Save((Join-Path $OutDir ("shot-{0:000}s.png" -f $t)))
    $bmp.Dispose()
}

if ($proc.HasExited) {
    "EXITED after {0:N1}s with code 0x{1:X8}" -f $clock.Elapsed.TotalSeconds, $proc.ExitCode
} else {
    "ALIVE after {0:N1}s; closing" -f $clock.Elapsed.TotalSeconds
    Stop-Process -Id $proc.Id -Force
}
