@echo off
rem Called by build scripts: leave VCVARS in the caller's setlocal environment.
rem An explicit VCVARS override supports nonstandard installations.
if defined VCVARS if exist "%VCVARS%" exit /b 0
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSWHERE_OUT=%TEMP%\winkick_vswhere_%RANDOM%_%RANDOM%.tmp"
if exist "%VSWHERE%" "%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSWHERE_OUT%" 2>nul
set "VSPATH="
if exist "%VSWHERE_OUT%" set /p VSPATH=<"%VSWHERE_OUT%"
del /q "%VSWHERE_OUT%" >nul 2>&1
if defined VSPATH if exist "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%VSPATH%\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles(x86)%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS if exist "%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat" set "VCVARS=%ProgramFiles%\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS goto :missing
exit /b 0
:missing
echo [build] MSVC x64 tools not found. Install the C++ Build Tools workload or set VCVARS.
exit /b 1
