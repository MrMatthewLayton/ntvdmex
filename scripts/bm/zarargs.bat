@echo off
rem zarargs.bat -- ZAR under NTVDMEX WITH A COMMAND LINE, then a screenshot and the log.
rem
rem   zarargs.bat 90 -Help          -> ZAR.EXE -Help, 90 s, shot + log
rem   zarargs.bat 420 -NoVESA2      -> the VESA-2 fallback path
rem   zarargs.bat 120 -NoSound
rem
rem WHY THIS EXISTS. ZAR.EXE's own help text (obj3+0x1b5 in the LE image) documents
rem switches nothing on this rig could reach, because every runner hard-codes a bare
rem `ZAR.EXE`:
rem     -Help       List of command-line options
rem     -NoSound    Completely disables sound system
rem     -NoVESA2    Disables VESA 2.0 linear frame buffer modes
rem     -Join <address> <port#> / -Psw <password>
rem Session 59 established that ZAR RUNS -- it cycles a 1073-read attract-demo loop
rem every ~175 s -- and simply never initialises video. Halving the video path with
rem -NoVESA2 is the cheapest question to ask of that, and -Help is the smoke test that
rem argument passing reaches the guest at all (it should print the block above and exit).
rem
rem ⚠ THE ARGUMENTS GO ON THE REAL COMMAND LINE, NOT IN target.txt. When CSRSS names the
rem   program -- which is what an IFEO-routed `start ZAR.EXE ...` does -- the host does
rem   NOT consult target.txt (see STAGE2: "CSRSS named a program"). target.txt is still
rem   written, for the case where the launch arrives without one.
rem ⚠ STDOUT IS REDIRECTED TO A FILE, because half the point is to READ WHAT ZAR SAYS.
rem   `-Help` prints and exits, and a `start`ed console vanishes with the program, so a
rem   screenshot would catch nothing. Same argument as zarout.bat, which exists because
rem   "stock runs ZAR" once rested on an entirely black BitBlt.
rem ⚠ THE autoexit MARKER IS SET, and that is safe here BECAUSE stdout is a file: the
rem   host exits with its guest (right for -Help, which terminates) and simply stays up
rem   when the guest does not (right for -NoVESA2, which is when we want the shot).
rem ⚠ STILL NO `start /wait`: on a host that keeps its window open /wait never returns
rem   and wedges the box (measured, s58 -- it took a controld kill to clear, twice).
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\zarargs.txt
set WAIT=%1
if "%WAIT%"=="" set WAIT=90
shift

rem Collect the rest of the line as the guest's arguments.
set GARGS=
:more
if "%1"=="" goto ready
set GARGS=%GARGS% %1
shift
goto more
:ready

del /q "%RES%\zarargs_done.txt" "%RES%\zarargs.bmp" "%RES%\zarargs_host.txt" >nul 2>&1
del /q "%RES%\zarargs_out.txt" C:\game\o_args.txt >nul 2>&1
> "%LOG%" echo ==== zarargs: %DATE% %TIME%  (wait %WAIT%s, args:%GARGS%) ====

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1
rem ⚠⚠ CLEAR THE CRASH-RECOVERY COUNTER. GH #132 counts CONSECUTIVE FAILED STARTS in
rem    C:\ntvdmex\startfail.txt, incremented at every start and cleared only by a CLEAN
rem    GUEST EXIT -- and an investigative run deliberately leaves the host up to be
rem    taskkill'd by the next one, which is not a clean exit. Three of those in a row
rem    and the host DECIDES IT IS THE PROBLEM and REMOVES ITS OWN IFEO KEY, after which
rem    every later run silently measures STOCK ntvdm. Measured, session 59: a run came
rem    back `start mode was UNINSTALL`, the guest terminated in 31 ms, and the evidence
rem    read as "-Help makes ZAR exit instantly" when the truth was "we uninstalled
rem    ourselves". The mechanism is right and should stay; a runner that kills the host
rem    ON PURPOSE just has no business feeding it.
del /q C:\ntvdmex\startfail.txt >nul 2>&1

reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo C:\game\ZAR.EXE%GARGS%> C:\ntvdmex\target.txt
echo.> C:\ntvdmex\autoexit
cd /d C:\game
start "" cmd /c "C:\game\ZAR.EXE%GARGS% > C:\game\o_args.txt 2>&1"
ping -n %WAIT% 127.0.0.1 >nul

rem ⚠ FORCE A REPAINT BEFORE THE SHOT -- the desktop keeps stale pixels, and session 57
rem   read that as a paint defect twice before shooting after `fg`.
"%BM%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\zarargs.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1
dir C:\ntvdmex\ntvdmhost.log >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\zarargs_host.txt" >nul 2>&1
copy /y C:\game\o_args.txt "%RES%\zarargs_out.txt" >nul 2>&1
echo --- ZAR stdout --- >> "%LOG%"
type C:\game\o_args.txt >> "%LOG%" 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1
echo [zarargs] LEFT RUNNING -- stop it from the tray icon or its own window. >> "%LOG%"
echo DONE > "%RES%\zarargs_done.txt"
