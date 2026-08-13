@echo off
rem anrun.bat -- runner for the GH#18 real-CPU PM ANIMATION (animate.com, milestone #6).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0animate.com" %N%\ >nul
echo %N%\animate.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
echo Running animate (real-CPU PM animation); screendump twice to see motion...
start /wait "" "%~dp0dosstub.com"
endlocal
