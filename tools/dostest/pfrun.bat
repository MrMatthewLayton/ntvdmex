@echo off
rem pfrun.bat -- one-shot runner for the GH #18 PM-fault reflect probe (pmfault.com, run 59).
rem Mirrors i31run.bat: points the IFEO Debugger straight at the CD's ntvdmhost.exe (fresh
rem host, no copy-to-C: lock), sets target.txt to pmfault, and triggers ONE run via dosstub.
rem The probe switches to PM then executes HLT (a raw PM #GP); read vm/serial.log on the host
rem for "GH#18: PM-FAULT REFLECTED to trampoline" (success) or a silent terminate (still walled).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0pmfault.com" %N%\ >nul
echo %N%\pmfault.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running pmfault (GH #18 PM-fault reflect probe) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 7 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
