@echo off
rem GH #1 DPMI 16-bit spike -- headless harness.
rem Points the IFEO Debugger at the CD's ntvdmhost.exe DIRECTLY (D:\), so every run uses
rem the freshly-built host -- no copy-to-C: that silently fails when a prior spinning
rem ntvdmhost still locks the file (the stale-host bug). The CD's unique volume label
rem defeats XP's CD cache. ntvdmhost logs to COM1 -> vm/serial.log (read on the host);
rem an external killer terminates the (un-self-terminable) PM guest so the log flushes.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0dpmitest.com" %N%\ >nul
echo %N%\dpmitest.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running GH#1 DPMI 16-bit switch spike (headless; read vm/serial.log)...
start "" /b cmd /c "ping -n 7 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "%~dp0dosstub.com"
endlocal
