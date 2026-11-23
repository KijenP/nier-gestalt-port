# Builds NierTextureTool.exe (window) and NierTextureTool-cli.exe (command line) with the C# compiler that ships with Windows.
param([string]$Out = (Join-Path $PSScriptRoot '..\..\out\build\win-renut'))
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$src = Get-ChildItem $PSScriptRoot -Filter *.cs | ForEach-Object { $_.FullName }
$refs = '/codepage:65001', '/reference:System.dll', '/reference:System.Drawing.dll', '/reference:System.Core.dll', '/reference:System.Windows.Forms.dll'
$exe = Join-Path $Out 'NierTextureTool.exe'
& $csc /nologo /target:winexe /optimize /out:$exe @refs $src
if ($LASTEXITCODE -eq 0) { "ok: $exe" }
$cli = Join-Path $Out 'NierTextureTool-cli.exe'
& $csc /nologo /target:exe /optimize /out:$cli @refs $src
if ($LASTEXITCODE -eq 0) { "ok: $cli" }