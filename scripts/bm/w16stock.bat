@echo off
rem ============================================================================
rem  w16stock.bat -- run a Win16 probe under STOCK ntvdm, to turn our numbers
rem                  into verdicts.
rem
rem     w16stock.bat <Folder> <EXE>
rem
rem  tools/wintest/ probes print values (GetVersion, GetWinFlags, ...) that are
rem  OURS until another machine has been asked. The documented authority for
rem  everything WOW is stock ntvdm on this same box, and the only way to reach it
rem  is to drop the IFEO Debugger value so the launch routes to stock instead of
rem  to us.
rem
rem  ⛔⛔⛔ THE IFEO KEY IS THE HAZARD, AND IT IS THE WHOLE RISK OF THIS SCRIPT.
rem    * Left ABSENT, every later test on this box silently measures stock ntvdm
rem      and the logs look entirely plausible while grading the wrong emulator.
rem    * Restored to the WRONG PATH, the box is worse than that: an IFEO Debugger
rem      pointing at a binary that does not exist makes every DOS and Win16
rem      launch fail, which is how this rig has been bricked before.
rem  ⚠ stockdump.bat CANNOT BE REUSED HERE for exactly that reason: it restores a
rem    hardcoded C:\ntvdmex\ntvdmhost.exe, which is NOT where this rig's host
rem    lives. This one restores the value w16launch.bat asserts, and w16launch
rem    re-asserts it at the start of every run as a second net.
rem  The before/after key is written to debug\out\w16stock_state.txt and the
rem  caller FAILS LOUDLY unless the "after" line is there.
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set T=%1
set EXE=%2
set S=%OUT%\w16stock_state.txt

if not exist "%W16%\%T%\%EXE%" (
  echo NO SUCH APP: %W16%\%T%\%EXE% > "%S%"
  echo done > "%SH%\w16stock_done.txt"
  goto :eof
)

rem -- clean slate; a stale artefact is worse than a missing one.
del /q "%OUT%\W16OUT.TXT" >nul 2>&1
del /q "%SH%\w16stock_done.txt" >nul 2>&1

taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1

echo ---- IFEO before ---- > "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- IFEO removed, running STOCK ---- >> "%S%"

cd /d "%W16%\%T%"
start "" "%W16%\%T%\%EXE%"
ping -n 16 127.0.0.1 >nul

echo ---- tasklist at run time ---- >> "%S%"
tasklist /fi "imagename eq ntvdm.exe"     >> "%S%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%S%" 2>&1

taskkill /f /im ntvdm.exe >nul 2>&1

rem -- RESTORE, UNCONDITIONALLY, to exactly what w16launch.bat asserts, and prove it.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
echo ---- IFEO after ---- >> "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (
  echo ---- restored target EXISTS ---- >> "%S%"
) else (
  echo ---- ****** RESTORED TARGET IS MISSING -- THE RIG IS BROKEN ****** ---- >> "%S%"
)
echo done > "%SH%\w16stock_done.txt"
