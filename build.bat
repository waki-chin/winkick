@echo off
setlocal
cd /d "%~dp0"
if not exist build\obj mkdir build\obj
rem Remove only this build's previous executable, never arbitrary user objects.
if exist build\winkick.exe del /q build\winkick.exe
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\generate-version.ps1 || exit /b 1
call scripts\find-vs.cmd || exit /b 1
echo [build] Using "%VCVARS%"
call "%VCVARS%" >nul || exit /b 1
echo [build] Compiling resources...
rem Resolve resource payloads from res, never an ignored root tasks.ini export.
pushd res || exit /b 1
rc /nologo /I . /I ..\build\generated /fo ..\build\obj\app.res app.rc
set "RC_RESULT=%ERRORLEVEL%"
popd
if not "%RC_RESULT%"=="0" exit /b %RC_RESULT%
echo [build] Compiling WinKick x64, static CRT...
cl /nologo /O2 /EHsc /MT /W4 /WX /GS /guard:cf /std:c++17 /utf-8 ^
    /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
    /Fobuild\obj\ /I src /I res /I build\generated ^
    src\main.cpp src\cli.cpp src\config.cpp src\net.cpp src\env.cpp src\runner.cpp src\gui.cpp ^
    build\obj\app.res /Fe:build\winkick.exe ^
    /link /SUBSYSTEM:WINDOWS /MANIFEST:NO /OPT:REF /OPT:ICF /INCREMENTAL:NO /Brepro ^
    /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /guard:cf ^
    /DELAYLOAD:winhttp.dll /DELAYLOAD:wininet.dll /DELAYLOAD:bcrypt.dll ^
    user32.lib gdi32.lib comctl32.lib shell32.lib comdlg32.lib winhttp.lib wininet.lib advapi32.lib ole32.lib bcrypt.lib delayimp.lib || exit /b 1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\write-toolchain.ps1 || exit /b 1
echo [build] OK: build\winkick.exe
endlocal
exit /b 0
