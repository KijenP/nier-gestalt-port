@echo off
rem Build the game (Release) and always refresh the SDK runtime DLLs next to it:
rem CMake only copies them when nier.exe relinks, so an SDK-only change would
rem otherwise leave stale DLLs behind.
setlocal
set "PROJ=%~dp0.."
if not defined REXSDK_INSTALL if exist "%PROJ%\.sdk\install\bin\rexglue.exe" set "REXSDK_INSTALL=%PROJ%\.sdk\install"
if not defined REXSDK_INSTALL set "REXSDK_INSTALL=%PROJ%\..\sdk-src\out\install\win-amd64"
call "%~dp0env.cmd" || exit /b 1
cd /d "%PROJ%" || exit /b 1
rem generated/ is never committed, and CMakeLists.txt includes the
rem generated/rexglue.cmake that codegen writes, so a fresh clone needs codegen
rem before CMake can configure at all.
if not exist generated\rexglue.cmake (
  "%REXSDK_INSTALL%\bin\rexglue.exe" codegen nier_manifest.toml || exit /b 1
)
rem rexglue_DIR is cached, so point it at this SDK explicitly every time.
rem Renut (the native renderer) on; NIER_GAME_PATH (the player's ISO or folder, set by setup.ps1) lets the build
rem take the shaders in the game's executable (tools\shader_extract); the rest are captured while playing.
cmake --preset win-amd64-release -DCMAKE_PREFIX_PATH="%REXSDK_INSTALL%" -Drexglue_DIR="%REXSDK_INSTALL%\lib\cmake\rexglue" -DNIER_RENUT=ON -DNIER_GAME_PATH="%NIER_GAME_PATH%" || exit /b 1
cmake --build --preset win-amd64-release || exit /b 1
for %%D in (rexruntime.dll rexgpu-xenos.dll) do (
  cmake -E copy_if_different "%REXSDK_INSTALL%\bin\%%D" out\build\win-amd64-release\ || exit /b 1
)
