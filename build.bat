@echo off
setlocal
cd /d "%~dp0"

if not exist build mkdir build

windres -i resources\SystemRPM_icon.rc -o build\SystemRPM_icon.o
if errorlevel 1 exit /b %errorlevel%

g++ -std=c++17 -O2 -Wall -Wextra -municode -mwindows ^
    src\anti_afk_mouse.cpp build\SystemRPM_icon.o -o build\SystemRMP.exe ^
    -lgdiplus -lcomdlg32 -luxtheme -luser32 -lgdi32
if errorlevel 1 exit /b %errorlevel%

copy /Y build\SystemRMP.exe SystemRMP.exe >nul
if errorlevel 1 exit /b %errorlevel%

echo Built SystemRMP.exe
