@echo off
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set R=%OUT%\result_w16watch.log
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
> "%R%" echo == w16watch notepad %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%SH%\bin\ntvdmhost.exe\"" /f >nul
echo "%SH%\demo\win16\notepad\NOTEPAD.EXE"> "%SH%\cfg\target.txt"
cd /d "%SH%\demo\win16\notepad"
start "" "%SH%\demo\win16\notepad\NOTEPAD.EXE"
for /L %%i in (1,1,20) do (
  >> "%R%" echo --- t=%%i %TIME%
  tasklist /fi "imagename eq ntvdmhost.exe" /fo csv /nh >> "%R%" 2>&1
  tasklist /fi "imagename eq ntvdm.exe" /fo csv /nh >> "%R%" 2>&1
  for %%f in ("%OUT%\ntvdmhost.log") do >> "%R%" echo log=%%~zf
  if %%i==3 copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_w16watch_t3.log" >nul 2>&1
  if %%i==6 copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_w16watch_t6.log" >nul 2>&1
  ping -n 2 127.0.0.1 >nul
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
>> "%R%" echo done %TIME%
