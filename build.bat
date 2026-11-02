@echo off
rem Builds build\jarvis.exe with MinGW-w64 gcc (needs gcc + windres on PATH).
setlocal
cd /d "%~dp0"
if not exist build mkdir build

windres res\jarvis.rc -O coff -o build\jarvis_res.o
if errorlevel 1 goto :fail

rem nomanifest.specs: works around MinGW installs in paths with spaces
rem (the default manifest object breaks the linker); we ship our own manifest.
gcc -std=c11 -Os -Wall -Wextra -Wno-unused-parameter -municode -mwindows ^
  -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 ^
  -specs=tools\nomanifest.specs ^
  src\main.c src\ui.c src\commands.c src\search.c src\voice.c src\dialog.c src\config.c src\util.c ^
  build\jarvis_res.o -o build\jarvis.exe ^
  -lole32 -loleaut32 -luuid -lshell32 -lshlwapi -lgdi32 -luser32 -ldwmapi -lcomctl32 -s
if errorlevel 1 goto :fail

echo.
echo Built build\jarvis.exe
exit /b 0

:fail
echo.
echo BUILD FAILED
exit /b 1
