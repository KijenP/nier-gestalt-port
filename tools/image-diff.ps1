# Pixel difference between two screenshots of the same size: share of
# pixels whose RGB differs by more than -Threshold, and the mean difference.
#
#   image-diff.ps1 a.png b.png [-Threshold 8]
param([Parameter(Mandatory)][string]$A, [Parameter(Mandatory)][string]$B, [int]$Threshold = 8)

Add-Type -AssemblyName System.Drawing
$ia = [Drawing.Bitmap]::FromFile($A); $ib = [Drawing.Bitmap]::FromFile($B)
try {
    if ($ia.Width -ne $ib.Width -or $ia.Height -ne $ib.Height) { throw "size differs" }
    $rect = New-Object Drawing.Rectangle 0, 0, $ia.Width, $ia.Height
    $da = $ia.LockBits($rect, 'ReadOnly', 'Format32bppArgb'); $db = $ib.LockBits($rect, 'ReadOnly', 'Format32bppArgb')
    $n = $da.Stride * $ia.Height
    $ba = New-Object byte[] $n; $bb = New-Object byte[] $n
    [Runtime.InteropServices.Marshal]::Copy($da.Scan0, $ba, 0, $n); [Runtime.InteropServices.Marshal]::Copy($db.Scan0, $bb, 0, $n)
    $ia.UnlockBits($da); $ib.UnlockBits($db)
    $changed = 0; $sum = 0.0; $pixels = $n / 4
    for ($i = 0; $i -lt $n; $i += 4) {
        $d = [math]::Max([math]::Max([math]::Abs($ba[$i] - $bb[$i]), [math]::Abs($ba[$i + 1] - $bb[$i + 1])), [math]::Abs($ba[$i + 2] - $bb[$i + 2]))
        $sum += $d; if ($d -gt $Threshold) { $changed++ }
    }
    [pscustomobject]@{ changed_pct = [math]::Round(100.0 * $changed / $pixels, 3); mean_diff = [math]::Round($sum / $pixels, 3) }
} finally { $ia.Dispose(); $ib.Dispose() }
