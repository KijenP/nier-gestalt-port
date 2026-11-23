# Builds NierTradutor.exe with the C# compiler that ships with Windows.
# Usage: build.ps1 [-Out <folder with nier.exe>]
param([string]$Out = (Join-Path $PSScriptRoot '..\..\out\build\win-renut'))
$csc = 'C:\Windows\Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$exe = Join-Path $Out 'NierTradutor.exe'
& $csc /nologo /target:winexe /optimize /out:$exe `
  /resource:"$PSScriptRoot\translator.html,translator.html" `
  /reference:System.dll "$PSScriptRoot\Translator.cs"
if ($LASTEXITCODE -eq 0) { "ok: $exe" }
