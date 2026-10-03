@echo off
rem ============================================================================
rem  w16pair.bat <Folder> <Caption> [EXE] -- ONE Win16 program under OURS, then
rem  under STOCK ntvdm, each screenshot at rest, one after the other. (s89, #162)
rem
rem  The question it answers is "what SHOULD this look like on this box": colours,
rem  layout, dialog placement -- things the log cannot say and memory of Win 3.1
rem  gets wrong (XP's COLOR_WINDOW is white, its button face beige). Sequential,
rem  not side by side, so both windows open at the same place and the two shots
rem  line up for a pixel diff.
rem
rem  Out: debug\out\pair_<Folder>_ours.bmp / _stock.bmp, pair_<Folder>.txt (the
rem       window list from each -- geometry included), pair_<Folder>_host.log.
rem       pair_done.txt when finished.
rem
rem  ⛔⛔⛔ THE IFEO KEY: removed for the stock half, restored UNCONDITIONALLY to the
rem    value w16launch.bat asserts, read back, and the target checked to exist --
rem    exactly w16stock.bat's discipline. A key left absent makes every later run
rem    measure stock; one pointing at a missing file bricks every DOS launch.
rem  ⚠ The caller must run hostcheck.bat first: this kills every ntvdmhost/ntvdm.
rem ============================================================================
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
set R=%OUT%\pair_%T%.txt
set L=%SH%\debug\ctl\rigshot.txt
del /q "%OUT%\pair_done.txt" "%OUT%\pair_%T%_*.bmp" >nul 2>&1
> "%R%" echo == w16pair %T% [%CAP%]  %DATE% %TIME%

taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\ntvdmhost.log" >nul 2>&1

rem ---- 1. OURS
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%W16%\%T%"
start "" "%W16%\%T%\%EXE%"
ping -n 16 127.0.0.1 >nul
>> "%R%" echo -- OURS: windows
del /q "%L%" >nul 2>&1
"%RIG%\rigshot.exe" list
type "%L%" >> "%R%" 2>&1
"%RIG%\rigshot.exe" shot "%OUT%\pair_%T%_ours.bmp" >nul 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\pair_%T%_host.log" >nul 2>&1
"%RIG%\rigshot.exe" close "%CAP%" >nul 2>&1
ping -n 4 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
ping -n 3 127.0.0.1 >nul

rem ---- 2. STOCK
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
>> "%R%" echo -- IFEO removed, STOCK:
start "" "%W16%\%T%\%EXE%"
ping -n 16 127.0.0.1 >nul
del /q "%L%" >nul 2>&1
"%RIG%\rigshot.exe" list
type "%L%" >> "%R%" 2>&1
"%RIG%\rigshot.exe" shot "%OUT%\pair_%T%_stock.bmp" >nul 2>&1
"%RIG%\rigshot.exe" close "%CAP%" >nul 2>&1
ping -n 4 127.0.0.1 >nul
taskkill /f /im ntvdm.exe >nul 2>&1

rem ---- RESTORE, UNCONDITIONALLY, and prove it
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
>> "%R%" echo -- IFEO after:
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (>> "%R%" echo -- restored target EXISTS) else (>> "%R%" echo ****** RESTORED TARGET IS MISSING -- THE RIG IS BROKEN ******)
>> "%R%" echo == done %TIME%
> "%OUT%\pair_done.txt" echo done
