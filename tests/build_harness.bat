@echo off
setlocal
cd /d "%~dp0.."
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\generate-version.ps1 || exit /b 1
call scripts\find-vs.cmd || exit /b 1
call "%VCVARS%" >nul || exit /b 1
cd /d "%~dp0"
if not exist out\obj mkdir out\obj
if exist out\runner_harness.exe del /q out\runner_harness.exe
cl /nologo /O2 /EHsc /MT /W4 /WX /std:c++17 /utf-8 ^
    /I ..\src /I ..\res /I ..\build\generated /DUNICODE /D_UNICODE /DNOMINMAX /D_WIN32_WINNT=0x0A00 ^
    /Foout\obj\ runner_harness.cpp ..\src\runner.cpp ..\src\config.cpp ..\src\env.cpp ..\src\cli.cpp ^
    /Fe:out\runner_harness.exe /link /INCREMENTAL:NO /Brepro ^
    kernel32.lib user32.lib advapi32.lib shell32.lib shlwapi.lib gdi32.lib || exit /b 1
echo [harness] OK: tests\out\runner_harness.exe
endlocal
exit /b 0
