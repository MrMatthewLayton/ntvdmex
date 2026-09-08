@echo off
rem zarplay.bat -- HAND ZAR TO A HUMAN. No log, no screenshot, no deadline.
rem
rem   zarplay.bat          -> ZAR.EXE, left running for you to play
rem   zarplay.bat -NoSound -> ...with a command line (arguments work as of s59)
rem
rem WHY THIS EXISTS, AND WHY IT IS NOT zarlong.bat. Session 59 got ZAR RENDERING: its
rem attract demo is on screen in mode 13h. Whether it is PLAYABLE is a different
rem question and this rig cannot answer it -- input latency, sound and "does it feel
rem right" need a person at the keyboard (see the headless-rig note in docs/STATE.md).
rem zarlong.bat is built for evidence: it screenshots, copies a multi-megabyte log and
rem leaves knobs armed. All of that COSTS THE GUEST SPEED, which is the one thing a
rem play test must not do.
rem
rem ⚠ THE TRACE IS SILENCED ON PURPOSE (wowquiet.txt). Measured this session: the
rem   reflected-INT trace alone was 53 MB in 45 s before it was bounded, and ZAR asks
rem   its own clock hook ~12,000 times a second. A play test wants the guest fast, not
rem   instrumented. **Delete wowquiet.txt afterwards** or every later run is silent --
rem   this script removes it again on the way out of the NEXT run, but say so anyway.
rem
rem WHAT TO TRY, and what is worth reporting back:
rem   * Does the attract demo play smoothly, or stutter?
rem   * Press ESC / SPACE / ENTER -- does it leave the demo and reach a MENU?
rem     (the binary has MAIN MENU / SELECT PLAYER / NEW PLAYER screens)
rem   * Arrow keys and the mouse -- does the pointer or selection move, and does it
rem     feel immediate or laggy?
rem   * Any sound at all? (expected: NONE -- the SB is never programmed, sb_dspwr=0)
rem   * If it dies, WHAT WAS ON SCREEN when it did.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

rem Collect any arguments for the guest.
set GARGS=
:more
if "%1"=="" goto ready
set GARGS=%GARGS% %1
shift
goto more
:ready

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1
rem ⚠⚠ GH #132 counts a deliberately-killed host as a FAILED START, and three in a row
rem    make the host remove its own IFEO key -- after which you would be playing STOCK
rem    ntvdm without being told. See the fuller note in zarargs.bat.
del /q C:\ntvdmex\startfail.txt >nul 2>&1
rem Silence the trace for speed; see above.
echo play test> "%RES%\wowquiet.txt"

reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo C:\game\ZAR.EXE%GARGS%> C:\ntvdmex\target.txt
cd /d C:\game
echo.
echo   Starting ZAR%GARGS% -- the window will open in a moment.
echo   Close it (or the tray icon) when you are done.
echo.
echo   NOTE: the host trace is SILENCED for speed. Delete
echo         "%RES%\wowquiet.txt" before the next diagnostic run.
echo.
start "" C:\game\ZAR.EXE%GARGS%
