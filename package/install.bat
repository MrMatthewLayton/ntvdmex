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
echo.
echo Installed. Every MS-DOS and 16-bit Windows program now runs under NTVDMEX.
echo   status.bat     shows which VDM is in force
echo   smoke.bat      runs the built-in self-test
echo   uninstall.bat  puts Windows' own ntvdm.exe back
echo.
pause
