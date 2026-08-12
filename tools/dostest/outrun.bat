@echo off
rem outrun.bat -- runner for the GH #18 PM I/O-virtualization probe (outprobe.com, RE session 8).
rem Points the IFEO Debugger at the CD's ntvdmhost.exe, sets target.txt to outprobe, triggers ONE
rem run via dosstub. The probe switches to PM then executes OUT DX,AL to VGA port 0x3C8/0x3C9.
rem Read vm/serial.log on the host: "OUT survived" = kernel emulated the I/O (guest resumed);
rem a stop at "about to OUT" = the OUT terminated the VDM (I/O not virtualized in PM).
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%~dp0ntvdmhost.exe" /f >nul
copy /y "%~dp0outprobe.com" %N%\ >nul
echo %N%\outprobe.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running outprobe (GH #18 PM I/O probe) once; read vm/serial.log on the host...
start "" /b cmd /c "ping -n 8 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start /wait "" "%~dp0dosstub.com"
endlocal
