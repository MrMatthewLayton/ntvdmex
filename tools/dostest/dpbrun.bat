@echo off
rem dpbrun.bat -- one-shot runner for the THIRD-PARTY DPMI client dpmiback.com
rem (Japheth, JWasm-built; run 50). Same pattern as i31run.bat: point the IFEO
rem Debugger at the CD's freshly-built ntvdmhost.exe, set target.txt to dpmiback,
rem trigger ONE run via dosstub. dpmiback exercises the real<->protected round-trips
rem (0300/0301) that i310102 never touched, so this probes whether the host PM
rem INTERPRETER (default since run 53) can subsume the kernel-BOP path that run 50
rem proved dpmiback on. Read the result in vm/serial.log on the host.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0dpmiback.com" %N%\ >nul
echo %N%\dpmiback.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running dpmiback (third-party DPMI client) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 7 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
