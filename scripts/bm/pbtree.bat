@echo off
rem pbtree.bat [x y] -- #161 Paintbrush's window layout, and optionally one click at CLIENT x y.
rem Launches PBRUSH, records every child window's position (rigshot tree) and a shot,
rem then -- given a client x y -- clicks there once and records the tree and a shot
rem again, so "did the click reach the toolbox or the canvas" is read off the log.
rem Report: debug\out\pbtree.txt (+ pbtree_*.bmp, pbtree_host.log). Closes Paintbrush.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set REP=%OUT%\pbtree.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == pbtree %1 %2  %DATE% %TIME%
del /q "%OUT%\pbtree_*" "%L%" "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%W16%\pbrush"
start "" "%W16%\pbrush\PBRUSH.EXE"
ping -n 18 127.0.0.1 >nul
"%R%" list
"%R%" tree "Paintbrush - (Untitled)"
"%R%" shot "%OUT%\pbtree_0.bmp"
if "%2"=="" goto done
"%R%" wclick "Paintbrush - (Untitled)" %1 %2
ping -n 3 127.0.0.1 >nul
"%R%" shot "%OUT%\pbtree_1.bmp"
:done
copy /y "%OUT%\ntvdmhost.log" "%OUT%\pbtree_host.log" >nul 2>&1
"%R%" close "Paintbrush - (Untitled)"
ping -n 6 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
type "%L%" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
