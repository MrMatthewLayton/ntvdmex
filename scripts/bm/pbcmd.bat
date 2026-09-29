@echo off
rem pbcmd.bat <id> [x y] -- #216: one Paintbrush menu command, alone, with its evidence.
rem Launches PBRUSH, shoots it, posts WM_COMMAND <id> to its frame (rigshot wcmd), then --
rem given a client x y -- clicks there (Zoom In waits for a click on the area to zoom),
rem shoots again, and keeps the host log. The log's UNIMPLEMENTED / STEPPED OVER lines
rem after the command are the calls it made that we do not answer.
rem Report: debug\out\pbcmd_<id>.txt (+ pbcmd_<id>_0/1.bmp, pbcmd_<id>_host.log).
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set C=Paintbrush - (Untitled)
set REP=%OUT%\pbcmd_%1.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == pbcmd %1 %2 %3  %DATE% %TIME%
del /q "%OUT%\pbcmd_%1_*" "%L%" "%OUT%\ntvdmhost.log" >nul 2>&1
cd /d "%W16%\pbrush"
start "" "%W16%\pbrush\PBRUSH.EXE"
ping -n 18 127.0.0.1 >nul
"%R%" shot "%OUT%\pbcmd_%1_0.bmp"
"%R%" wcmd "%C%" %1
ping -n 4 127.0.0.1 >nul
if not "%3"=="" "%R%" wclick "%C%" %2 %3
ping -n 4 127.0.0.1 >nul
"%R%" list
"%R%" shot "%OUT%\pbcmd_%1_1.bmp"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\pbcmd_%1_host.log" >nul 2>&1
"%R%" close "%C%"
ping -n 6 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
type "%L%" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
