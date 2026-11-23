@echo off
rem Toolchain environment for building the ReXGlue SDK and the NieR project.
rem
rem 2026-11-05: when this drive has its own copy of the toolchain (\VISUALSTUDIO and
rem \Windows Kits at the drive's root, copied from the E: install), the environment is
rem built from it directly, without the registry or vswhere: the build keeps working with
rem the drive Visual Studio was installed on removed. Otherwise, as before: Visual Studio
rem (C++ tools) found with vswhere, LLVM in Program Files or on PATH, and the VS-bundled
rem Ninja and Git put on PATH. Set VS_ROOT / LLVM_ROOT beforehand to override.

set "NIER_TC=%~d0"
set "NIER_MSVC_VER=14.51.36231"
set "NIER_SDK_VER=10.0.26100.0"
if exist "%NIER_TC%\VISUALSTUDIO\Community\VC\Tools\MSVC\%NIER_MSVC_VER%\bin\HostX64\x64\link.exe" if exist "%NIER_TC%\Windows Kits\10\Include\%NIER_SDK_VER%" goto local

if not defined VS_ROOT (
  for /f "usebackq delims=" %%I in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_ROOT=%%I"
)
if not defined VS_ROOT (
  echo [env] Visual Studio com "Desenvolvimento para desktop com C++" nao encontrado.
  exit /b 1
)
if not defined LLVM_ROOT if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "LLVM_ROOT=%ProgramFiles%\LLVM"

call "%VS_ROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
if defined LLVM_ROOT set "PATH=%LLVM_ROOT%\bin;%PATH%"
set "PATH=%PATH%;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd"

where clang++ >nul 2>nul || (echo [env] LLVM/clang nao encontrado. & exit /b 1)
exit /b 0

:local
set "VS_ROOT=%NIER_TC%\VISUALSTUDIO\Community"
set "VSINSTALLDIR=%VS_ROOT%\"
set "VCINSTALLDIR=%VS_ROOT%\VC\"
set "VCToolsVersion=%NIER_MSVC_VER%"
set "VCToolsInstallDir=%VS_ROOT%\VC\Tools\MSVC\%NIER_MSVC_VER%\"
set "WindowsSdkDir=%NIER_TC%\Windows Kits\10\"
set "WindowsSDKVersion=%NIER_SDK_VER%\"
set "WindowsSdkBinPath=%WindowsSdkDir%bin\"
set "WindowsSdkVerBinPath=%WindowsSdkDir%bin\%NIER_SDK_VER%\"
set "UniversalCRTSdkDir=%WindowsSdkDir%"
set "UCRTVersion=%NIER_SDK_VER%"
set "Platform=x64"
set "VSCMD_ARG_TGT_ARCH=x64"
set "VSCMD_ARG_HOST_ARCH=x64"
set "NIER_K=%WindowsSdkDir%Include\%NIER_SDK_VER%"
set "NIER_L=%WindowsSdkDir%Lib\%NIER_SDK_VER%"
set "INCLUDE=%VCToolsInstallDir%include;%VCToolsInstallDir%ATLMFC\include;%VS_ROOT%\VC\Auxiliary\VS\include;%NIER_K%\ucrt;%NIER_K%\um;%NIER_K%\shared;%NIER_K%\winrt;%NIER_K%\cppwinrt"
set "EXTERNAL_INCLUDE=%INCLUDE%"
set "LIB=%VCToolsInstallDir%ATLMFC\lib\x64;%VCToolsInstallDir%lib\x64;%NIER_L%\ucrt\x64;%NIER_L%\um\x64"
set "LIBPATH=%VCToolsInstallDir%ATLMFC\lib\x64;%VCToolsInstallDir%lib\x64"
if not defined LLVM_ROOT if exist "%ProgramFiles%\LLVM\bin\clang++.exe" set "LLVM_ROOT=%ProgramFiles%\LLVM"
set "PATH=%LLVM_ROOT%\bin;%VCToolsInstallDir%bin\HostX64\x64;%WindowsSdkVerBinPath%x64;%WindowsSdkBinPath%x64;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%VS_ROOT%\Common7\IDE\CommonExtensions\Microsoft\TeamFoundation\Team Explorer\Git\cmd;%PATH%"
where clang++ >nul 2>nul || (echo [env] LLVM/clang nao encontrado. & exit /b 1)
exit /b 0
