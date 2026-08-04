@echo off
rem i31run.bat -- one-shot runner for the C-runtime DPMI client (i310102.exe), runs 51+.
rem Mirrors dpmitest.bat: points the IFEO Debugger straight at the CD's ntvdmhost.exe (so
rem every run uses the freshly-built host, no copy-to-C: lock), sets target.txt to i310102,
rem and triggers ONE run via dosstub. Unlike the multi-client dpmiauto.bat, this never
rem chains a client that can hang the batch -- i310102 stops cleanly on the first unmodeled
rem opcode, so its serial output always lands. Read the result in vm/serial.log on the host.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0i310102.exe" %N%\ >nul
echo %N%\i310102.exe> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running i310102 (C-runtime DPMI client) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 7 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
