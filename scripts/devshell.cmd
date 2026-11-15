@echo off
rem Run inside an existing clang-cl + MSVC environment; never install toolchains.
setlocal
if not defined VSINSTALLDIR (
  set "PS3_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
  if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" exit /b 2
  for /f "usebackq delims=" %%P in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%P\VC\Auxiliary\Build\vcvars64.bat" >nul
)
if not defined VSINSTALLDIR exit /b 2
%*
exit /b %ERRORLEVEL%
