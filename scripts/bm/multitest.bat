@echo off
rem multitest.bat -- #211 two NTVDMEX hosts at once, and #153's "new window".
rem Phase 1: bare prompt (A) -> Open Recent #1 types QB into A -> Open Recent #2
rem          (Skyroads) while QB runs -> a NEW window B (instance 2). Then Exit B
rem          through its menu and check A is still alive.
rem Phase 2: with A still running QB, the harness's Skyroads run as instance 2
rem          (target.txt + autoexit, exactly rt.bat's :run) -> its V86STR timing line.
rem Report: debug\out\multitest.txt. Kills what it started; removes what it wrote.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set K=HKCU\Software\NTVDMEX
set SKY=%SH%\demo\msdos\skyroads
set REP=%OUT%\multitest.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == multitest %DATE% %TIME%
del /q "%OUT%\multitest_*.bmp" "%OUT%\ntvdmhost.log" "%OUT%\startfail.txt" "%SH%\rigshot.txt" >nul 2>&1
if exist "%OUT%\2" rd /s /q "%OUT%\2"
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.txt.multitest" >nul
del /q "%CFG%\autoexit" >nul 2>&1
for %%i in (1 2 3 4 5 6 7 8) do reg delete "%K%" /v Recent%%i /f >nul 2>&1
reg add "%K%" /v Recent1 /t REG_SZ /d "%SH%\demo\msdos\qb45\QB.EXE" /f >nul
reg add "%K%" /v Recent2 /t REG_SZ /d "%SKY%\SKYROADS.EXE" /f >nul

rem ---- phase 1 ----
start "" /D "%SH%" "%BIN%\ntvdmhost.exe"
ping -n 10 127.0.0.1 >nul
"%R%" cmd 180 >nul 2>&1
ping -n 8 127.0.0.1 >nul
>> "%REP%" echo -- 1a: QB typed into A; now Open Recent #2 (Skyroads) while QB runs
"%R%" cmd 181 >nul 2>&1
ping -n 15 127.0.0.1 >nul
"%R%" shot "%OUT%\multitest_1.bmp" >nul 2>&1
>> "%REP%" echo -- hosts with A and B (%TIME%):
tasklist | find /i "ntvdmhost" >> "%REP%"
>> "%REP%" echo -- B's log (debug\out\2):
find "STAGE2: instance" "%OUT%\2\ntvdmhost.log" >> "%REP%" 2>&1
>> "%REP%" echo -- 1b: File ^> Exit posted to the topmost VDM window (B)
"%R%" cmd 2 >nul 2>&1
ping -n 6 127.0.0.1 >nul
>> "%REP%" echo -- hosts after closing B:
tasklist | find /i "ntvdmhost" >> "%REP%"
"%R%" shot "%OUT%\multitest_2.bmp" >nul 2>&1

rem ---- phase 2 ----
>> "%REP%" echo -- 2: harness Skyroads as a second host, A still running
echo "%SKY%\SKYROADS.EXE"> "%CFG%\target.txt"
echo.> "%CFG%\autoexit"
cd /d "%SKY%"
start /wait "" "%RIG%\dosstub.com"
del /q "%CFG%\autoexit" "%CFG%\target.txt" >nul 2>&1
copy /y "%OUT%\2\ntvdmhost.log" "%OUT%\multitest_sky.log" >nul 2>&1
find "STAGE2: instance" "%OUT%\multitest_sky.log" >> "%REP%" 2>&1
find "V86STR" "%OUT%\multitest_sky.log" >> "%REP%" 2>&1
>> "%REP%" echo -- A still alive after B's run:
tasklist | find /i "ntvdmhost" >> "%REP%"
>> "%REP%" echo -- A's log (debug\out): instance + OPEN lines
find "STAGE2: instance" "%OUT%\ntvdmhost.log" >> "%REP%" 2>&1
find "OPEN:" "%OUT%\ntvdmhost.log" >> "%REP%" 2>&1

"%R%" close "Windows NT Virtual DOS Machine" >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" close "Windows NT Virtual DOS Machine" >nul 2>&1
ping -n 4 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
for %%i in (1 2 3 4 5 6 7 8) do reg delete "%K%" /v Recent%%i /f >nul 2>&1
del /q "%OUT%\startfail.txt" "%OUT%\2\startfail.txt" >nul 2>&1
if exist "%CFG%\target.txt.multitest" move /y "%CFG%\target.txt.multitest" "%CFG%\target.txt" >nul
>> "%REP%" echo == done %TIME%
endlocal
