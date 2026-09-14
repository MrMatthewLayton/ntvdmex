@echo off
setlocal
cd /d "%~dp0"
echo.
"%~dp0bm\ntvdmhost.exe" /status
echo.
if exist "%~dp0out\ntvdmhost.log" (
    echo Last run's log: %~dp0out\ntvdmhost.log
    echo Its first lines:
    echo -----------------------------------------------------------------------
    for /f "usebackq delims=" %%L in (`findstr /B /C:"STAGE0: root" /C:"STAGE1: program" /C:"STAGE2: exec loop" /C:"HOSTFAULT" "%~dp0out\ntvdmhost.log"`) do echo   %%L
    echo -----------------------------------------------------------------------
) else (
    echo No program has run under NTVDMEX yet (no out\ntvdmhost.log).
)
echo.
pause
