@echo off
rem dpmiinstall.bat -- ONE-TIME setup for the headless DPMI harness. Run once from the
rem XP desktop (double-click on the CD). Installs dpmiauto.bat to C:\ntvdmex and adds a
rem Run registry key so it fires on every login. Thereafter a DPMI test cycle is just:
rem   host:  build -> build-test-iso -> qmp.py cd <iso> -> qmp.py system_reset -> read vm/serial.log
rem No GUI navigation ever again.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%
copy /y "%~dp0dpmiauto.bat" %N%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" /v ntvdmex_dpmiauto /t REG_SZ /d "%N%\dpmiauto.bat" /f >nul
echo.
echo Installed dpmiauto to %N% and the Run key. The DPMI test now auto-runs on every
echo boot (when the DPMI CD is mounted). Reset the VM to try it.
echo.
pause
endlocal
