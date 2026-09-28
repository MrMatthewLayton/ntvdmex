@echo off
rem kbtest.bat -- #136 the keyboard layout setting, through Edit > Paste (typed) and
rem Copy Whole Screen. Selects United Kingdom (KeyboardLayout=1), pastes text holding
rem the characters UK moves, copies the screen back; restores the setting afterwards.
rem Report: debug\out\kbtest.txt
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set T=Microsoft Windows XP Virtual DOS Machine
set K=HKCU\Software\NTVDMEX
set REP=%OUT%\kbtest.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == kbtest %DATE% %TIME%
>> "%REP%" echo -- KeyboardLayout before:
reg query "%K%" /v KeyboardLayout >> "%REP%" 2>&1
rem Remember the value itself (a .reg export/import did NOT restore it on XP -- s82).
set KBV=
for /f "tokens=3" %%v in ('reg query "%K%" /v KeyboardLayout 2^>nul ^| find "KeyboardLayout"') do set KBV=%%v
reg add "%K%" /v KeyboardLayout /t REG_DWORD /d 1 /f >nul
del /q "%L%" >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.txt.kbtest" >nul
start "" /D "%SH%" "%BIN%\ntvdmhost.exe"
ping -n 10 127.0.0.1 >nul
"%R%" clipset "echo A@B#C~D"
"%R%" cmd 17
ping -n 4 127.0.0.1 >nul
"%R%" fg "%T%"
"%R%" key 13
ping -n 3 127.0.0.1 >nul
"%R%" clipset "EMPTY"
"%R%" cmd 16
ping -n 2 127.0.0.1 >nul
"%R%" clipget
copy /y "%OUT%\ntvdmhost.log" "%OUT%\kbtest_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
if exist "%CFG%\target.txt.kbtest" move /y "%CFG%\target.txt.kbtest" "%CFG%\target.txt" >nul
if defined KBV ( reg add "%K%" /v KeyboardLayout /t REG_DWORD /d %KBV% /f >nul ) else ( reg delete "%K%" /v KeyboardLayout /f >nul 2>&1 )
>> "%REP%" echo -- KeyboardLayout after (restored):
reg query "%K%" /v KeyboardLayout >> "%REP%" 2>&1
type "%L%" >> "%REP%" 2>&1
find "KeyboardLayout" "%OUT%\kbtest_host.log" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
