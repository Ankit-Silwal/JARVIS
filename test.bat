@echo off
rem Builds and runs the JARVIS test suite (MinGW-w64 gcc on PATH).
rem Usage: test.bat     -> exit code 0 when every test passes
setlocal
cd /d "%~dp0"
if not exist build\tests mkdir build\tests

set CFLAGS=-std=c11 -O1 -g -Wall -Wextra -Wno-unused-parameter -municode -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -specs=tools\nomanifest.specs
set LIBS=-lole32 -luuid -lshell32 -lshlwapi -luser32

echo === building test_util
gcc %CFLAGS% tests\test_util.c src\util.c -o build\tests\test_util.exe %LIBS%
if errorlevel 1 goto :fail

echo === building test_search
gcc %CFLAGS% tests\test_search.c src\search.c src\util.c src\config.c -o build\tests\test_search.exe %LIBS%
if errorlevel 1 goto :fail

echo === building test_docs_cmd
gcc %CFLAGS% tests\test_docs_cmd.c src\commands.c src\search.c src\dialog.c src\docs.c src\util.c src\config.c -o build\tests\test_docs_cmd.exe %LIBS% -loleaut32
if errorlevel 1 goto :fail

echo === building test_cmd
gcc %CFLAGS% tests\test_cmd.c src\commands.c src\search.c src\dialog.c src\docs.c src\util.c src\config.c -o build\tests\test_cmd.exe %LIBS% -loleaut32
if errorlevel 1 goto :fail

echo === building test_whisper
gcc %CFLAGS% tests\test_whisper.c src\whisper.c src\util.c src\config.c -o build\tests\test_whisper.exe %LIBS%
if errorlevel 1 goto :fail

echo === building test_docs (runs only with JARVIS_DOCS_TEST=1)
gcc %CFLAGS% tests\test_docs.c src\docs.c src\util.c src\config.c -o build\tests\test_docs.exe %LIBS% -loleaut32
if errorlevel 1 goto :fail

echo.
echo === running test_util
build\tests\test_util.exe
if errorlevel 1 goto :fail

echo.
echo === running test_search
build\tests\test_search.exe
if errorlevel 1 goto :fail

echo.
echo === running test_docs_cmd
build\tests\test_docs_cmd.exe
if errorlevel 1 goto :fail

echo.
echo === running test_cmd
build\tests\test_cmd.exe
if errorlevel 1 goto :fail

echo.
echo === running test_whisper
build\tests\test_whisper.exe
if errorlevel 1 goto :fail

echo.
echo === running test_docs
build\tests\test_docs.exe
if errorlevel 1 goto :fail

echo.
echo ALL TESTS PASSED
exit /b 0

:fail
echo.
echo TESTS FAILED
exit /b 1
