@echo off
rem ============================================================================
rem  w16launch.bat -- start a Windows 3.11 demo through the IFEO hook, by name.
rem
rem     w16launch.bat <Folder> [EXE] [keep]
rem
rem  Runs demo\win16\<Folder>\<EXE> (EXE defaults to <Folder>.EXE) exactly the way a
rem  double-click would: `start` it, and the ntvdm.exe Debugger value routes the
rem  launch into bin\ntvdmhost.exe. After ~15 s it lists the desktop's windows with
rem  rigshot (so the run can be judged without a human), copies the host log to
rem  debug\out\result_w16_<Folder>.log and takes the guest down -- unless `keep` is
rem  given, in which case it is left on screen for a person.  (s73)
rem
rem  Drive it via controld:
rem     exec cmd /c ""<share>\debug\rig\w16launch.bat" notepad"
rem     exec cmd /c ""<share>\debug\rig\w16launch.bat" pbrush PBRUSH.EXE keep"
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set T=%1
set EXE=%2
if "%EXE%"=="" set EXE=%T%.EXE
if /i "%EXE%"=="keep" set EXE=%T%.EXE& set KEEP=1
if /i "%3"=="keep" set KEEP=1
set R=%OUT%\result_w16_%T%.log
if not exist "%OUT%" md "%OUT%"
> "%R%" echo == w16launch %T% %EXE%  %DATE% %TIME%
if not exist "%W16%\%T%\%EXE%" (
  >> "%R%" echo NO SUCH APP: %W16%\%T%\%EXE%
  dir /b "%W16%" >> "%R%" 2>&1
  goto :eof
)

rem ⚠⚠ XP's WOW VDM IS SHARED: a live ntvdm.exe is handed the new 16-bit program and
rem    the IFEO hook never fires, so the app comes up under STOCK while everything
rem    looks fine. Kill both, every time.
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
rem GH #132: those kills are unclean exits; three in a row and the host uninstalls
rem its own key. Clear the counter and RE-ASSERT the key before every launch.
del /q "%OUT%\startfail.txt" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul

rem s73: the host now takes the Win16 program from CSRSS (GetNextVDMCommand with
rem VDM_FLAG_WOW), as stock WOW does. target.txt is NOT written here any more -- a
rem stale one is the negative control: if THAT program comes up, the fetch failed.
cd /d "%W16%\%T%"
>> "%R%" echo launching %W16%\%T%\%EXE% at %TIME%
start "" "%W16%\%T%\%EXE%"
ping -n 16 127.0.0.1 >nul

>> "%R%" echo --- processes:
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
tasklist /fi "imagename eq ntvdm.exe"     >> "%R%" 2>&1
rem rigshot logs to debug\ctl\rigshot.txt, not stdout (GUI-subsystem image).
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" list
ping -n 3 127.0.0.1 >nul
>> "%R%" echo --- windows:
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
>> "%R%" echo --- host log (STAGE lines):
if exist "%OUT%\ntvdmhost.log" (
  findstr /C:"STAGE0: root" /C:"STAGE1:" /C:"STAGE2: start mode" /C:"HOSTFAULT" /C:"WOW:" "%OUT%\ntvdmhost.log" >> "%R%"
  copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_w16_%T%_host.log" >nul 2>&1
) else (
  >> "%R%" echo *** NO HOST LOG -- stock ntvdm ran this, or nothing ran
)
if defined KEEP (
  >> "%R%" echo left running for a human at %TIME%
  goto :eof
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
>> "%R%" echo stopped at %TIME%
