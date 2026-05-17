@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 exit /b %errorlevel%

if "%1"=="configure" (
    cmake --preset x64-release
) else if "%1"=="run" (
    cmake --build out\build\x64-release --config Release
    if errorlevel 1 exit /b %errorlevel%
    start "GamepadMidiController" cmd /k out\build\x64-release\GamepadMidiController\GamepadMidiController.exe
) else (
    cmake --build out\build\x64-release --config Release
)
