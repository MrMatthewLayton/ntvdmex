@echo off
rem rjrun.bat -- one-shot runner for the third-party DPMI client rawjmp7.exe
rem (Japheth HX Regress16; tests raw protected-mode entry / far selector jumps).
rem Same pattern as i31run.bat / dpbrun.bat: point the IFEO Debugger at the CD's
rem freshly-built ntvdmhost.exe, set target.txt to rawjmp7, trigger ONE run via
rem dosstub. Broadens the interpreter-path corpus past i310102 (C runtime) and
rem dpmiback (0301 round-trip). Read the result in vm/serial.log on the host.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0rawjmp7.exe" %N%\ >nul
echo %N%\rawjmp7.exe> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running rawjmp7 (third-party DPMI client) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 7 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
