@echo off
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0pm32irq.com" %N%\ >nul
echo %N%\pm32irq.com> %N%\target.txt
start /wait "" "%~dp0dosstub.com"
endlocal
