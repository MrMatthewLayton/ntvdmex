@echo off
setlocal
cd /d "%~dp0"
echo.
echo NTVDMEX -- put the machine's own ntvdm.exe back
echo.
"%~dp0bm\ntvdmhost.exe" /uninstall
if errorlevel 1 (
    echo.
    echo UNINSTALL FAILED. If the message above says "access denied", run this from
    echo an administrator account: right-click uninstall.bat and choose Run as...
    pause
    exit /b 1
)
echo.
echo Done. MS-DOS and 16-bit Windows programs use Windows' own VDM again.
echo The folder can now be deleted; nothing else was written outside it except
echo the registry value that has just been removed.
echo.
pause
