@echo off
rem Run inside an existing clang-cl + MSVC environment; never install toolchains.
setlocal
if defined VSINSTALLDIR goto run
if not exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" exit /b 2
for /f "usebackq delims=" %%P in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PS3_VS=%%P"
if not defined PS3_VS exit /b 2
set "VSCMD_SKIP_SENDTELEMETRY=1"
set "VSCMD_VCVARSALL_INIT=1"
call "%PS3_VS%\Common7\Tools\VsDevCmd.bat" -no_logo -no_ext -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b %ERRORLEVEL%
call "%PS3_VS%\Common7\Tools\vsdevcmd\ext\vcvars.bat" >nul
if errorlevel 1 exit /b %ERRORLEVEL%
set "INCLUDE=%__VSCMD_VCVARS_INCLUDE%%INCLUDE%"
set "EXTERNAL_INCLUDE=%__VSCMD_VCVARS_INCLUDE%%EXTERNAL_INCLUDE%"
:run
%*
exit /b %ERRORLEVEL%
