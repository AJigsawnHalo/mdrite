@echo off
setlocal
set "WROOT=C:\WATCOM"
if defined WATCOM_ROOT set "WROOT=%WATCOM_ROOT%"
set "WATCOM=%WROOT%"
set "INCLUDE=%WROOT%\h"
set "PATH=%WROOT%\BINNT64;%WROOT%\BINNT;%PATH%"
cd /d "%~dp0"

if /i "%1"=="clean" goto clean

set "FLAGS=-d0 -os"
if /i "%1"=="debug" set "FLAGS=-d2"

if not exist build mkdir build
wcl -bt=dos -ml -0 -wx %FLAGS% -fe=mdrite.exe -fo=build\ *.c
if errorlevel 1 exit /b 1
echo OK -^> mdrite.exe
exit /b 0

:clean
if exist build rmdir /s /q build
del /q mdrite.exe *.err *.map *.obj 2>nul
echo Cleaned
exit /b 0
