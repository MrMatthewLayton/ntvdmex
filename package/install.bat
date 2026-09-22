@echo off
setlocal
cd /d "%~dp0"
echo.
echo NTVDMEX -- make this copy the machine's MS-DOS and Win16 VDM
echo Folder: %~dp0
echo.
if not exist "%~dp0bin\ntvdmhost.exe" (
    echo bin\ntvdmhost.exe is missing next to this script. Extract the whole zip.
    pause
    exit /b 1
)
"%~dp0bin\ntvdmhost.exe" /install
if errorlevel 1 (
    echo.
    echo INSTALL FAILED. If the message above says "access denied", this needs an
    echo administrator account: right-click install.bat and choose Run as...
    pause
    exit /b 1
)
rem ── VERIFY BY ASKING AGAIN, NOT BY TRUSTING THE EXIT CODE. ────────────────────
rem A zero exit code from /install used to be reachable without anything being
rem written (the single-instance guard swallowed the verb when any guest was on
rem screen), and this script then announced success -- reported from the field on
rem Windows 2000. /status re-reads the registry and classifies it: 0 = ours.
"%~dp0bin\ntvdmhost.exe" /status >nul
if errorlevel 1 (
    echo.
    echo INSTALL DID NOT TAKE. The registry does not name NTVDMEX as this machine's
    echo VDM even though the step above reported no error. Run status.bat to see who
    echo owns it, close any MS-DOS or 16-bit Windows program that is running, and
    echo try again.
    pause
    exit /b 1
)
echo.
echo Installed. Every MS-DOS and 16-bit Windows program now runs under NTVDMEX.
echo   status.bat     shows which VDM is in force
echo   smoke.bat      runs the built-in self-test
echo   uninstall.bat  puts Windows' own ntvdm.exe back
echo.
pause
