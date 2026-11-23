# Removes every icon resource (RT_ICON and RT_GROUP_ICON) from an EXE.
# The local build embeds game\nier.ico (the icon from the player's own disc); a copy handed to anyone else must not
# carry it. The game sets the disc icon at run time, after the player picks their ISO.
# Usage: scripts\remove-exe-icon.ps1 -Exe "path\NieR Gestalt.exe"
param([Parameter(Mandatory)][string]$Exe)
$ErrorActionPreference = 'Stop'
$Exe = (Resolve-Path $Exe).Path

Add-Type -Namespace W -Name R -MemberDefinition @'
public delegate bool EnumNameProc(System.IntPtr h, System.IntPtr type, System.IntPtr name, System.IntPtr param);
public delegate bool EnumLangProc(System.IntPtr h, System.IntPtr type, System.IntPtr name, ushort lang, System.IntPtr param);
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr LoadLibraryEx(string f, System.IntPtr r, uint flags);
[System.Runtime.InteropServices.DllImport("kernel32")] public static extern bool FreeLibrary(System.IntPtr h);
[System.Runtime.InteropServices.DllImport("kernel32")] public static extern bool EnumResourceNames(System.IntPtr h, System.IntPtr type, EnumNameProc p, System.IntPtr param);
[System.Runtime.InteropServices.DllImport("kernel32")] public static extern bool EnumResourceLanguages(System.IntPtr h, System.IntPtr type, System.IntPtr name, EnumLangProc p, System.IntPtr param);
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError=true, CharSet=System.Runtime.InteropServices.CharSet.Unicode)]
public static extern System.IntPtr BeginUpdateResource(string f, bool deleteAll);
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError=true)]
public static extern bool UpdateResource(System.IntPtr u, System.IntPtr type, System.IntPtr name, ushort lang, System.IntPtr data, uint size);
[System.Runtime.InteropServices.DllImport("kernel32", SetLastError=true)] public static extern bool EndUpdateResource(System.IntPtr u, bool discard);
'@

# Only integer ids are expected (the .rc uses "1 ICON"); named ones are reported and left.
$found = New-Object System.Collections.ArrayList
$h = [W.R]::LoadLibraryEx($Exe, [IntPtr]::Zero, 0x2)  # LOAD_LIBRARY_AS_DATAFILE
if ($h -eq [IntPtr]::Zero) { throw "could not open $Exe" }
foreach ($type in 3, 14) {  # RT_ICON, RT_GROUP_ICON
  $names = New-Object System.Collections.ArrayList
  [void][W.R]::EnumResourceNames($h, [IntPtr]$type, { param($x, $t, $n, $p) [void]$names.Add($n); $true }, [IntPtr]::Zero)
  foreach ($n in $names) {
    if ($n.ToInt64() -gt 0xFFFF) { Write-Warning "named icon resource left in place"; continue }
    [void][W.R]::EnumResourceLanguages($h, [IntPtr]$type, $n, { param($x, $t, $nn, $l, $p) [void]$found.Add(@($type, $nn, $l)); $true }, [IntPtr]::Zero)
  }
}
[void][W.R]::FreeLibrary($h)
if ($found.Count -eq 0) { "No icon in $Exe"; return }

$u = [W.R]::BeginUpdateResource($Exe, $false)
if ($u -eq [IntPtr]::Zero) { throw "BeginUpdateResource failed" }
foreach ($r in $found) {
  if (-not [W.R]::UpdateResource($u, [IntPtr]$r[0], $r[1], [uint16]$r[2], [IntPtr]::Zero, 0)) {
    [void][W.R]::EndUpdateResource($u, $true); throw "UpdateResource failed"
  }
}
if (-not [W.R]::EndUpdateResource($u, $false)) { throw "EndUpdateResource failed" }
"Removed $($found.Count) icon resources from $Exe"
