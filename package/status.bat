@echo off
setlocal
cd /d "%~dp0"
echo.
"%~dp0bin\ntvdmhost.exe" /status
echo.
if exist "%~dp0debug\out\ntvdmhost.log" (
    echo Last run's log: %~dp0debug\out\ntvdmhost.log
    echo Its first lines:
    echo -----------------------------------------------------------------------
    for /f "usebackq delims=" %%L in (`findstr /B /C:"STAGE0: root" /C:"STAGE1: program" /C:"STAGE2: exec loop" /C:"HOSTFAULT" "%~dp0debug\out\ntvdmhost.log"`) do echo   %%L
    echo -----------------------------------------------------------------------
) else (
    echo No program has run under NTVDMEX yet (no debug\out\ntvdmhost.log).
)
echo.
pause
