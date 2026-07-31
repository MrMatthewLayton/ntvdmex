@echo off
rem GH #14 regression test -- run from the XP DESKTOP off the CD.
rem Copies the host + modeswitch.com to C:\ntvdmex, points the IFEO at our host,
rem and runs the mode-13h -> text-3 switch. The Luna window must show READABLE
rem text after the switch (black/blank = the bug is back).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

copy /y "%~dp0ntvdmhost.exe" %N%\ >nul
copy /y "%~dp0dosstub.com"   %N%\ >nul
copy /y "%~dp0modeswitch.com" %N%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f >nul

echo %N%\modeswitch.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running GH#14 mode-switch test... a Luna window will open in mode 13h,
echo hold ~1s, then return to text and print a message you must be able to READ.
start /wait %N%\dosstub.com

echo.
echo ============ ntvdmhost.log ============
type %N%\ntvdmhost.log 2>nul
echo =======================================
pause
endlocal
