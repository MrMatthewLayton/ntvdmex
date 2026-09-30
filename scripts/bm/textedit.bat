@echo off
rem textedit.bat -- #154 the host window's Edit menu in text mode, driven by WM_COMMAND.
rem   16 = Copy Whole Screen, 17 = Paste (typed as keys), 18 = Select All, 15 = Copy.
rem Opens a bare NTVDMEX prompt, copies the screen, pastes `echo PASTE154 OK` + Enter,
rem copies the screen again (the echo's output must be in it), then Select All + Copy.
rem Report: debug\out\textedit.txt (+ textedit_*.bmp). Closes the host; restores cfg.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set T=Windows NT Virtual DOS Machine
set REP=%OUT%\textedit.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == textedit %DATE% %TIME%
del /q "%OUT%\textedit_*.bmp" "%L%" >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.txt.textedit" >nul
start "" /D "%SH%" "%BIN%\ntvdmhost.exe"
ping -n 10 127.0.0.1 >nul
"%R%" clipset "EMPTY"
"%R%" cmd 16
ping -n 2 127.0.0.1 >nul
"%R%" clipget
"%R%" clipset "echo PASTE154 OK"
"%R%" cmd 17
ping -n 4 127.0.0.1 >nul
"%R%" fg "%T%"
"%R%" key 13
ping -n 3 127.0.0.1 >nul
"%R%" shot "%OUT%\textedit_1.bmp"
"%R%" clipset "EMPTY"
"%R%" cmd 16
ping -n 2 127.0.0.1 >nul
"%R%" clipget
"%R%" clipset "EMPTY"
"%R%" cmd 18
"%R%" cmd 15
ping -n 2 127.0.0.1 >nul
"%R%" clipget
taskkill /f /im ntvdmhost.exe >nul 2>&1
if exist "%CFG%\target.txt.textedit" move /y "%CFG%\target.txt.textedit" "%CFG%\target.txt" >nul
type "%L%" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
