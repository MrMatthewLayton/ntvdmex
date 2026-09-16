@echo off
rem w16close.bat <Folder> <Caption> [EXE] -- does the X actually close a Win16 app? (s73)
rem User-reported: "Charmap ... X does not close either", "WinMine works, X does not close".
rem Launches the app, lists windows, sends WM_CLOSE the way the X button does
rem (rigshot close), waits, then lists again and reports whether the window AND the
rem host process are gone. The host log is kept either way -- what the guest did with
rem WM_CLOSE (DestroyWindow? WM_DESTROY? PostQuitMessage?) is the whole question.
rem Report: debug\out\w16close_<Folder>.txt ; w16close_done.txt says it finished.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set T=%1
set CAP=%~2
set EXE=%3
if "%EXE%"=="" set EXE=%T%.EXE
set R=%OUT%\w16close_%T%.txt
del /q "%OUT%\w16close_done.txt" >nul 2>&1
> "%R%" echo == w16close %T% [%CAP%]  %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%W16%\%T%"
start "" "%W16%\%T%\%EXE%"
ping -n 16 127.0.0.1 >nul
>> "%R%" echo -- windows BEFORE close:
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 3 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
>> "%R%" echo -- sending WM_CLOSE to [%CAP%]:
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" close "%CAP%"
ping -n 3 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
ping -n 11 127.0.0.1 >nul
>> "%R%" echo -- windows AFTER close (10s later):
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 3 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
>> "%R%" echo -- host process still alive?
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
>> "%R%" echo -- what the guest did with WM_CLOSE:
findstr /c:"WM_CLOSE" /c:"DestroyWindow" /c:"WM_DESTROY" /c:"PostQuitMessage" /c:"GetMessage -> WM_QUIT" /c:"WM_QUIT" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\w16close_%T%_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
>> "%R%" echo == done %TIME%
> "%OUT%\w16close_done.txt" echo done
