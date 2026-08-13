@echo off
rem m13run.bat -- runner for the GH#18 PM VGA render slice (mode13.com, milestone #6).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0mode13.com" %N%\ >nul
echo %N%\mode13.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
echo Running mode13 (PM VGA render) once; screendump the ntvdmhost window...
start /wait "" "%~dp0dosstub.com"
endlocal
