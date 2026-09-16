@echo off
setlocal
cd /d "%~dp0"
echo.
echo NTVDMEX smoke test -- runs the built-in self-test under the installed VDM.
echo Expect eight lines ending in PASS. It takes a few seconds.
echo.
rem /status answers in its EXIT CODE: 0 = NTVDMEX is the machine's VDM, 1 = nobody
rem is, 2 = another program is. This used to grep the output for a sentence that
rem only install.bat prints, so it reported "not installed" immediately after
rem install.bat reported success.
"%~dp0bin\ntvdmhost.exe" /status >nul
if errorlevel 1 (
    echo NTVDMEX is not installed on this machine. Run install.bat first.
    echo (status.bat will say who owns the VDM.^)
    pause
    exit /b 1
)
"%~dp0bin\selftest.com"
echo.
if not exist "%~dp0debug\out\ntvdmhost.log" (
    echo FAIL: no log was written to debug\out\ -- NTVDMEX did not run at all.
    echo Run status.bat and send its output.
    pause
    exit /b 1
)
findstr /C:"STAGE2: exec loop exited" "%~dp0debug\out\ntvdmhost.log" >nul
if errorlevel 1 (
    echo FAIL: the run did not end cleanly. Send debug\out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"HOSTFAULT" "%~dp0debug\out\ntvdmhost.log" >nul
if not errorlevel 1 (
    echo FAIL: the host faulted. Send debug\out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"FAIL=" "%~dp0debug\out\ntvdmhost.log" >nul
if not errorlevel 1 (
    echo FAIL: one or more self-test subsystems failed -- see the report above.
    echo Send debug\out\ntvdmhost.log.
    pause
    exit /b 1
)
findstr /C:"ALL TESTS PASSED" "%~dp0debug\out\ntvdmhost.log" >nul
if errorlevel 1 (
    echo FAIL: the self-test did not report ALL TESTS PASSED. Send debug\out\ntvdmhost.log.
    pause
    exit /b 1
)
echo OK: all eight self-test subsystems PASSED under NTVDMEX.
pause
