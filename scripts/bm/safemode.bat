@echo off
rem safemode.bat -- #132: start the host with the failure counter at 2 (SAFE MODE) and
rem run debug\tests\selftest\selftest.com; report the counter before and after.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\safemode.txt
del /q "%OUT%\safemode_done.txt" >nul 2>&1
> "%R%" echo == safemode %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
> "%OUT%\startfail.txt" echo %1
> "%SH%\cfg\autoexit" echo 1
>> "%R%" echo counter before: & type "%OUT%\startfail.txt" >> "%R%"
cd /d "%SH%\debug\tests\selftest"
start "" "%SH%\debug\tests\selftest\selftest.com"
ping -n 45 127.0.0.1 >nul
>> "%R%" echo counter after:
if exist "%OUT%\startfail.txt" (type "%OUT%\startfail.txt" >> "%R%") else (>> "%R%" echo [absent = cleared by a clean exit])
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\safemode_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
echo done > "%OUT%\safemode_done.txt"
