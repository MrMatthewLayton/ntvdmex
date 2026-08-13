@echo off
rem iorun.bat -- runner for the GH #18 PM I/O round-trip verify (ioverify.com, run 73 follow-up).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0ioverify.com" %N%\ >nul
echo %N%\ioverify.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
echo Running ioverify (PM DAC round-trip) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 8 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
