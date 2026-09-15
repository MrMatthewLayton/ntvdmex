@echo off
setlocal
cd /d "%~dp0"
echo.
echo NTVDMEX smoke test -- runs the built-in self-test under the installed VDM.
echo Expect eight lines ending in PASS. It takes a few seconds.
echo.
"%~dp0bm\ntvdmhost.exe" /status | findstr /I /C:"installed as this machine" >nul
if errorlevel 1 (
    echo NTVDMEX is not installed on this machine. Run install.bat first.
    pause
    exit /b 1
)
"%~dp0bm\selftest.com"
echo.
if not exist "%~dp0out\ntvdmhost.log" (
    echo FAIL: no log was written to out\ -- NTVDMEX did not run at all.
    echo Run status.bat and send its output.
    pause
    exit /b 1
)
findstr /C:"STAGE2: exec loop exited" "%~dp0out\ntvdmhost.log" >nul
if errorlevel 1 (
    echo FAIL: the run did not end cleanly. Send out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"HOSTFAULT" "%~dp0out\ntvdmhost.log" >nul
if not errorlevel 1 (
    echo FAIL: the host faulted. Send out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"FAIL=" "%~dp0out\ntvdmhost.log" >nul
if not errorlevel 1 (
    echo FAIL: one or more self-test subsystems failed -- see the report above.
    echo Send out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"ALL TESTS PASSED" "%~dp0out\ntvdmhost.log" >nul
if errorlevel 1 (
    echo FAIL: the self-test did not report ALL TESTS PASSED. Send out\ntvdmhost.log.
    pause
    exit /b 1
)
echo OK: all eight self-test subsystems PASSED under NTVDMEX.
pause
