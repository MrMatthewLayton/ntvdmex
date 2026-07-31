@echo off
rem GH #1 DPMI 16-bit spike -- run from the XP DESKTOP off the CD.
rem Copies the host + dpmitest.com to C:\ntvdmex, points the IFEO at our host, and
rem runs the real->protected-mode switch. The interesting output is in the dumped
rem ntvdmhost.log: look for "DPMI switch ... -> PM ok" then "STAGE3-DPMI: PM stop".
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

copy /y "%~dp0ntvdmhost.exe" %N%\ >nul
copy /y "%~dp0dosstub.com"   %N%\ >nul
copy /y "%~dp0dpmitest.com"  %N%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f >nul

echo %N%\dpmitest.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running GH#1 DPMI 16-bit switch spike...
start /wait %N%\dosstub.com

echo.
echo ============ ntvdmhost.log ============
type %N%\ntvdmhost.log 2>nul
echo =======================================
pause
endlocal
