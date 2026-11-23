@echo off
rem Play with the controller while every input is recorded.
rem Usage: record.cmd [name]   -> recordings\<name>.csv (default: sessao)
setlocal
set "ROOT=%~dp0..\.."
set "NAME=%~1"
if "%NAME%"=="" set "NAME=sessao"
if not exist "%ROOT%\recordings" mkdir "%ROOT%\recordings"
rem Never overwrite an earlier recording: sessao.csv, sessao-2.csv, sessao-3.csv...
set "NIER_INPUT_RECORD=%ROOT%\recordings\%NAME%.csv"
set /a N=1
:next
if exist "%NIER_INPUT_RECORD%" (
  set /a N+=1
  call set "NIER_INPUT_RECORD=%ROOT%\recordings\%NAME%-%%N%%.csv"
  goto next
)
rem Snapshot the starting state next to the recording (save data + settings),
rem so replays start exactly where this session did. The originals are only read.
set "STATE=%NIER_INPUT_RECORD:.csv=.state%"
mkdir "%STATE%\userdata" 2>nul
if exist "%USERPROFILE%\Documents\nier\B13EBABEBABEBABE" robocopy "%USERPROFILE%\Documents\nier\B13EBABEBABEBABE" "%STATE%\userdata\B13EBABEBABEBABE" /E /NFL /NDL /NJH /NJS >nul
if exist "%ROOT%\nier-port\out\build\win-amd64-release\nier.toml" copy /y "%ROOT%\nier-port\out\build\win-amd64-release\nier.toml" "%STATE%\nier.toml" >nul
echo Gravando em %NIER_INPUT_RECORD%
echo Feche a janela do jogo quando terminar.
rem --headless answers system dialogs (e.g. the name prompt) with their default
rem text, so recordings never depend on keyboard input and replay stays in sync.
"%ROOT%\nier-port\out\build\win-amd64-release\nier.exe" --game_data_root="%ROOT%\NieR_Extraido" --fullscreen=true --headless=true --headless_keyboard_text=Nier %NIER_EXTRA_ARGS%
echo Gravacao salva: %NIER_INPUT_RECORD%
pause
