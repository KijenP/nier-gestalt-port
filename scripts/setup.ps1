# NieR Gestalt PC - setup window.
# The user picks their own copy of the game (an .iso or a folder holding
# default.xex). Everything else is automatic: validate the copy, check the
# build tools, build the ReXGlue SDK (pinned tag + our patch), generate and
# compile the game code, remember where the game is, and add a desktop shortcut.
param([switch]$NoGui, [string]$GamePath, [switch]$NoShortcut)  # -NoGui -GamePath <iso|folder>: same steps, console only

$ErrorActionPreference = 'Stop'
$Repo = Split-Path $PSScriptRoot
$SdkTag = 'v0.10.0'
$SdkUrl = 'https://github.com/rexglue/rexglue-sdk.git'
$TitleId = '535107E8'
$LogFile = Join-Path $Repo 'out\setup.log'
New-Item -ItemType Directory -Force (Split-Path $LogFile) | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type -Path (Join-Path $Repo 'tools\GameImage.cs') -ReferencedAssemblies System.Drawing

# --------------------------------------------------------------------------
# Steps (run on a background runspace; report through $sync)
# --------------------------------------------------------------------------
$steps = {
    param($sync, $Repo, $GamePath, $SdkTag, $SdkUrl, $TitleId, $LogFile)
    $ErrorActionPreference = 'Stop'

    function Say([string]$status, [int]$step) {
        $sync.Status = $status; $sync.Step = $step
        Add-Content -LiteralPath $LogFile -Value "== $status" -Encoding utf8
    }
    # Each command writes to its own file (appending several redirections to the
    # shared log from one cmd line trips a sharing violation).
    function Run([string]$what, [string]$cmdline, [switch]$NoThrow) {
        Add-Content -LiteralPath $LogFile -Value "> $cmdline" -Encoding utf8
        $out = "$LogFile.step"
        cmd /c "($cmdline) > `"$out`" 2>&1"
        $rc = $LASTEXITCODE
        if (Test-Path $out) { Get-Content $out | Add-Content -LiteralPath $LogFile -Encoding utf8; Remove-Item $out }
        if ($rc -ne 0 -and -not $NoThrow) { throw "$what falhou (código $rc). Veja os detalhes." }
        return $rc
    }
    $env_ = "call `"$Repo\scripts\env.cmd`""

    try {
        # 1. The copy of the game.
        Say 'Verificando sua cópia do jogo...' 1
        if (Test-Path -LiteralPath $GamePath -PathType Leaf) {
            $xex = [GameImage]::ReadDefaultXex($GamePath)
        } elseif (Test-Path -LiteralPath (Join-Path $GamePath 'default.xex')) {
            $xex = [IO.File]::ReadAllBytes((Join-Path $GamePath 'default.xex'))
        } else {
            throw 'Essa pasta não tem o default.xex. Escolha a pasta onde a ISO foi extraída (a que contém default.xex e media).'
        }
        $info = [GameImage]::TitleInfo($xex).Split(',')
        if ($info[0] -ne $TitleId) {
            throw "Essa cópia não é o NieR de Xbox 360 (Title ID $($info[0]); esperado $TitleId)."
        }
        New-Item -ItemType Directory -Force (Join-Path $Repo 'game') | Out-Null
        [IO.File]::WriteAllBytes((Join-Path $Repo 'game\default.xex'), $xex)
        Add-Content -LiteralPath $LogFile -Value "title=$($info[0]) media=$($info[1])" -Encoding utf8

        # 2. Build tools.
        Say 'Verificando as ferramentas de compilação...' 2
        if ((Run 'Ferramentas' "$env_ && where cmake ninja git clang++" -NoThrow) -ne 0) {
            $sync.MissingTools = $true; throw 'Faltam ferramentas de compilação.'
        }

        # 3. ReXGlue SDK at the pinned tag, with our patches.
        $patch = Join-Path $Repo 'patches\rexglue-v0.10.0.diff'
        $patchHash = (Get-FileHash -LiteralPath $patch -Algorithm SHA256).Hash
        $install = if ($env:REXSDK_INSTALL) { $env:REXSDK_INSTALL } else { Join-Path $Repo '.sdk\install' }
        $stamp = Join-Path $install 'nier-patch.sha256'
        if (-not $env:REXSDK_INSTALL -and -not ((Test-Path $stamp) -and (Get-Content $stamp) -eq $patchHash)) {
            $src = Join-Path $Repo '.sdk\src'
            if (-not (Test-Path (Join-Path $src '.git'))) {
                Say 'Baixando o ReXGlue SDK (uma vez só)...' 3
                Run 'Baixar o SDK' "$env_ && git -c advice.detachedHead=false clone --branch $SdkTag --depth 1 --recurse-submodules --shallow-submodules $SdkUrl `"$src`"" | Out-Null
            }
            Say 'Aplicando as correções do SDK...' 3
            # Already applied? (A failing reverse check here is the normal case.)
            if ((Run 'Patch' "$env_ && git -C `"$src`" apply --check -R `"$patch`" 2>nul" -NoThrow) -ne 0) {
                Run 'Aplicar o patch do SDK' "$env_ && git -C `"$src`" apply `"$patch`"" | Out-Null
            }
            Run 'Corrigir links do SDK' "$env_ && powershell -NoProfile -ExecutionPolicy Bypass -File `"$Repo\scripts\fix-symlinks.ps1`" -SdkRoot `"$src`"" | Out-Null
            Say 'Compilando o ReXGlue SDK (pode levar 10-20 minutos)...' 3
            Run 'Configurar o SDK' "$env_ && cd /d `"$src`" && cmake --preset win-amd64 -DCMAKE_INSTALL_PREFIX=`"$install`"" | Out-Null
            Run 'Compilar o SDK' "$env_ && cd /d `"$src`" && cmake --build --preset win-amd64-release" | Out-Null
            Run 'Instalar o SDK' "$env_ && cd /d `"$src`" && cmake --install out/build/win-amd64 --config Release" | Out-Null
            Set-Content -LiteralPath $stamp -Value $patchHash
        }

        # 4. The icon: the title tile from the user's own disc (nxeart), never shipped; game\nier.ico goes into the
        # exe (CMakeLists.txt) and the shortcut.
        $ico = Join-Path $Repo 'game\nier.ico'
        try {
            $art = if (Test-Path -LiteralPath $GamePath -PathType Leaf) { [GameImage]::ReadRootFile($GamePath, 'nxeart') }
                   else { [IO.File]::ReadAllBytes((Join-Path $GamePath 'nxeart')) }
            $png = [GameImage]::TileFromNxeArt($art)
            if ($png) { [IO.File]::WriteAllBytes($ico, [GameImage]::IcoFromPng($png)) }
        } catch {
            Add-Content -LiteralPath $LogFile -Value "sem ícone do disco: $($_.Exception.Message)" -Encoding utf8
        }

        # 5. Game code: generate from default.xex and compile (Renut, the shaders from the disc's executable).
        Say 'Gerando e compilando o código do jogo (pode levar vários minutos)...' 4
        $gameFull = (Resolve-Path -LiteralPath $GamePath).Path.Replace('\', '/')
        Run 'Compilar o jogo' "set `"REXSDK_INSTALL=$install`" && set `"NIER_GAME_PATH=$gameFull`" && `"$Repo\scripts\build.cmd`"" | Out-Null
        $exeDir = Join-Path $Repo 'out\build\win-amd64-release'
        $exe = Join-Path $exeDir 'NieR Gestalt.exe'
        if (-not (Test-Path -LiteralPath $exe)) { throw 'A compilação terminou mas "NieR Gestalt.exe" não apareceu.' }
        # The translations (traducao\<language>: only the files they change; the rest come from the disc) go to TEXT,
        # where Options > Language lists them.
        foreach ($lang in Get-ChildItem -LiteralPath (Join-Path $Repo 'traducao') -Directory -ErrorAction SilentlyContinue) {
            Copy-Item -LiteralPath $lang.FullName -Destination (Join-Path $exeDir 'TEXT') -Recurse -Force
        }

        # 5. Remember where the game is (nier.toml next to the exe).
        Say 'Salvando a localização do jogo...' 5
        $toml = Join-Path $exeDir 'nier.toml'
        $lines = @(if (Test-Path $toml) { Get-Content $toml | Where-Object { $_ -notmatch '^\s*game_data_root\s*=' } })
        # Forward slashes: the SDK re-saves nier.toml with basic "..." strings
        # without escaping backslashes, which then fails to parse ("\N").
        $full = (Resolve-Path -LiteralPath $GamePath).Path.Replace('\', '/')
        Set-Content -LiteralPath $toml -Encoding utf8 -Value (@("game_data_root = '$full'") + $lines)

        # 6. Desktop shortcut.
        Say 'Criando o atalho na área de trabalho...' 6
        $lnk = Join-Path ([Environment]::GetFolderPath('Desktop')) 'NieR Gestalt.lnk'
        if ($sync.NoShortcut) { $lnk = Join-Path $Repo 'out\NieR Gestalt.lnk' }
        $icon = "$exe,0"  # the exe carries the disc's icon (step 4)
        $sc = (New-Object -ComObject WScript.Shell).CreateShortcut($lnk)
        $sc.TargetPath = $exe; $sc.WorkingDirectory = $exeDir; $sc.IconLocation = $icon
        $sc.Description = 'NieR Gestalt (PC)'
        $sc.Save()

        $sync.Exe = $exe
        Say 'Pronto! Use o atalho "NieR Gestalt" na área de trabalho.' 7
        $sync.Done = $true
    } catch {
        # Calls into GameImage arrive wrapped in a MethodInvocationException.
        $e = $_.Exception
        while ($e.InnerException) { $e = $e.InnerException }
        Add-Content -LiteralPath $LogFile -Value "ERRO: $($e.Message)" -Encoding utf8
        $sync.Error = $e.Message
    }
}

$missingToolsHelp = @'
Para compilar o jogo no seu PC são necessárias estas ferramentas gratuitas.
Abra o "Terminal" do Windows e cole uma linha de cada vez:

winget install Microsoft.VisualStudio.2022.BuildTools --override "--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install LLVM.LLVM --version 20.1.8
winget install Kitware.CMake
winget install Git.Git

Depois abra o Preparar.cmd de novo.
'@

# --------------------------------------------------------------------------
# Console mode (for testing and automation)
# --------------------------------------------------------------------------
if ($NoGui) {
    Set-Content -LiteralPath $LogFile -Value "setup $(Get-Date -Format s) path=$GamePath" -Encoding utf8
    $sync = [hashtable]::Synchronized(@{ NoShortcut = [bool]$NoShortcut })
    & $steps $sync $Repo $GamePath $SdkTag $SdkUrl $TitleId $LogFile
    if ($sync.Error) { Write-Host "ERRO: $($sync.Error)"; if ($sync.MissingTools) { Write-Host $missingToolsHelp }; exit 1 }
    Write-Host $sync.Status; exit 0
}

# --------------------------------------------------------------------------
# Window
# --------------------------------------------------------------------------
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()

$form = New-Object Windows.Forms.Form
$form.Text = 'NieR Gestalt - Preparar'
$form.ClientSize = New-Object Drawing.Size(560, 250)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'; $form.MaximizeBox = $false
$form.Font = New-Object Drawing.Font('Segoe UI', 10)
$form.BackColor = [Drawing.Color]::FromArgb(24, 24, 24); $form.ForeColor = [Drawing.Color]::Gainsboro

$title = New-Object Windows.Forms.Label
$title.Text = 'NieR Gestalt'; $title.Font = New-Object Drawing.Font('Georgia', 20)
$title.AutoSize = $true; $title.Location = New-Object Drawing.Point(24, 18)
$form.Controls.Add($title)

$hint = New-Object Windows.Forms.Label
$hint.Text = 'Localize sua cópia do jogo: o arquivo .iso ou a pasta extraída (com default.xex).'
$hint.Size = New-Object Drawing.Size(512, 40); $hint.Location = New-Object Drawing.Point(26, 66)
$form.Controls.Add($hint)

function New-Button([string]$text, [int]$x, [int]$w) {
    $b = New-Object Windows.Forms.Button
    $b.Text = $text; $b.Location = New-Object Drawing.Point($x, 112); $b.Size = New-Object Drawing.Size($w, 36)
    $b.FlatStyle = 'Flat'; $b.BackColor = [Drawing.Color]::FromArgb(48, 48, 48)
    $form.Controls.Add($b); $b
}
$btnIso = New-Button 'Escolher ISO...' 26 160
$btnDir = New-Button 'Escolher pasta...' 196 160
$btnPlay = New-Button 'Jogar' 378 160; $btnPlay.Visible = $false

$status = New-Object Windows.Forms.Label
$status.Size = New-Object Drawing.Size(512, 44); $status.Location = New-Object Drawing.Point(26, 160)
$form.Controls.Add($status)

$bar = New-Object Windows.Forms.ProgressBar
$bar.Location = New-Object Drawing.Point(26, 206); $bar.Size = New-Object Drawing.Size(400, 18)
$bar.Maximum = 7; $bar.Visible = $false
$form.Controls.Add($bar)

$details = New-Object Windows.Forms.LinkLabel
$details.Text = 'Detalhes'; $details.AutoSize = $true; $details.Location = New-Object Drawing.Point(452, 206)
$details.LinkColor = [Drawing.Color]::Silver; $details.Visible = $false
$details.add_LinkClicked({ if (Test-Path $LogFile) { Start-Process notepad.exe $LogFile } })
$form.Controls.Add($details)

$sync = [hashtable]::Synchronized(@{})
$timer = New-Object Windows.Forms.Timer
$timer.Interval = 300

function Start-Setup([string]$path) {
    $btnIso.Enabled = $false; $btnDir.Enabled = $false
    $bar.Visible = $true; $details.Visible = $true
    Set-Content -LiteralPath $LogFile -Value "setup $(Get-Date -Format s) path=$path" -Encoding utf8
    $sync.Clear()
    $ps = [PowerShell]::Create()
    $ps.AddScript($steps).AddArgument($sync).AddArgument($Repo).AddArgument($path).AddArgument($SdkTag).
        AddArgument($SdkUrl).AddArgument($TitleId).AddArgument($LogFile) | Out-Null
    $ps.Runspace.SessionStateProxy.SetVariable('Repo', $Repo)
    $script:job = $ps.BeginInvoke(); $script:ps = $ps
    $timer.Start()
}

$timer.add_Tick({
    if ($sync.Status) { $status.Text = $sync.Status }
    if ($sync.Step) { $bar.Value = [Math]::Min([int]$sync.Step, $bar.Maximum) }
    if ($sync.Done) {
        $timer.Stop(); $bar.Visible = $false; $btnPlay.Visible = $true
    } elseif ($sync.Error) {
        $timer.Stop(); $bar.Visible = $false
        $status.Text = $sync.Error
        $btnIso.Enabled = $true; $btnDir.Enabled = $true
        if ($sync.MissingTools) {
            [Windows.Forms.MessageBox]::Show($missingToolsHelp, 'Ferramentas necessárias') | Out-Null
        }
    }
})

$btnIso.add_Click({
    $dlg = New-Object Windows.Forms.OpenFileDialog
    $dlg.Filter = 'Imagem de disco (*.iso)|*.iso|Todos os arquivos|*.*'
    $dlg.Title = 'Escolha a ISO do NieR (Xbox 360)'
    if ($dlg.ShowDialog() -eq 'OK') { Start-Setup $dlg.FileName }
})
$btnDir.add_Click({
    $dlg = New-Object Windows.Forms.FolderBrowserDialog
    $dlg.Description = 'Escolha a pasta extraída do NieR (a que contém default.xex)'
    if ($dlg.ShowDialog() -eq 'OK') { Start-Setup $dlg.SelectedPath }
})
$btnPlay.add_Click({ Start-Process -FilePath $sync.Exe -WorkingDirectory (Split-Path $sync.Exe); $form.Close() })

[void]$form.ShowDialog()
