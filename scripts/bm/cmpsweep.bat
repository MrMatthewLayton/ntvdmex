@echo off
rem cmpsweep.bat -- EVERY GUEST, UNDER BOTH HOSTS, CAPTURED THE SAME WAY.
rem GH #128, session 57. The user's method, and it is the right one:
rem
rem   "the best way to test all of the apps is stock NTVDM side-by-side with NTVDMEX"
rem
rem wowcompare.bat already does ONE program under both and leaves them on screen for
rem a human. This does the whole shelf, unattended, and captures each half SEPARATELY
rem so the two can be pixel-diffed rather than eyeballed -- which is how session 53
rem settled the "our Win16 chrome is wrong" report (0 of 13,440 pixels differed) and
rem how two of the last three reported defects were refuted.
rem
rem   cmpsweep.bat                 -- the default shelf list below
rem   cmpsweep.bat CALC CARDFILE   -- just those
rem
rem PER GUEST, IN THIS ORDER:
rem   1. kill everything, re-add the IFEO value, launch under NTVDMEX, shoot, dump
rem      the window tree (which is what gives the differ its crop rectangle)
rem   2. kill everything, REMOVE the IFEO value, launch under stock, shoot, dump
rem   3. RESTORE the IFEO value and PROVE it with reg query
rem
rem ⚠⚠ THE IFEO VALUE IS RE-ADDED BEFORE EVERY GUEST, not once at the top. GH #132
rem   counts consecutive unclean starts and DROPS the Debugger value at three --
rem   and every `taskkill /f` below manufactures one. Session 54 lost two runs to
rem   this: the key goes, and the rest of the sweep silently measures stock ntvdm
rem   on BOTH halves while producing entirely plausible logs.
rem ⚠⚠ `start ""`, NEVER `start /wait`. Stock ntvdm raises a MODAL DIALOG for a
rem   program it cannot run (WINFILE does exactly that here), and a batch waiting on
rem   that is a wedged rig that only a hand at the box can clear.
rem ⚠ ASK THE HOST TO CLOSE BEFORE KILLING IT, or its tray icon outlives it as a
rem   ghost -- an externally terminated host never runs Shell_NotifyIcon(NIM_DELETE).
setlocal ENABLEDELAYEDEXPANSION
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\cmpsweep.txt
set LIST=%*
if "%LIST%"=="" set LIST=CALC CARDFILE WRITE PROGMAN PACKAGER SOUNDREC CLOCK TASKMAN

del /q "%RES%\cmpsweep_done.txt" >nul 2>&1
del /q "%RES%\cmp_*.bmp" >nul 2>&1
del /q "%RES%\cmp_*.txt" >nul 2>&1
> "%LOG%" echo ==== cmpsweep: NTVDMEX vs STOCK, %DATE% %TIME% ====
echo list: %LIST% >> "%LOG%"

if not exist C:\ntvdmex md C:\ntvdmex
rem -- one deploy for the whole sweep; each guest re-copies nothing, so a host left
rem    holding the file cannot make a later guest run a stale binary.
"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
(echo 0)> "%RES%\wowidle.txt"
if not exist "%RES%\wowsched.txt" type nul > "%RES%\wowsched.txt"
if not exist "%RES%\wowcall.txt"  type nul > "%RES%\wowcall.txt"

for %%G in (%LIST%) do call :one %%G

reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo. >> "%LOG%"
echo ==== the IFEO key, as left ==== >> "%LOG%"
reg query "%IFEO%" /v Debugger >> "%LOG%" 2>&1
echo DONE > "%RES%\cmpsweep_done.txt"
goto :eof

:one
set G=%1
echo. >> "%LOG%"
echo ================ %G% ================ >> "%LOG%"

rem ---- 1. OURS -------------------------------------------------------------
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
call :stop
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
echo C:\WIN16\%G%.EXE> C:\ntvdmex\target.txt
start "" C:\WIN16\%G%.EXE
ping -n 16 127.0.0.1 >nul
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (echo   !! OURS NOT RUNNING -- went to stock, or exited >> "%LOG%") else (echo   ours: host alive >> "%LOG%")
rem ⚠ FORCE A REPAINT BEFORE THE SHOT. The desktop keeps stale pixels from earlier
rem   runs, and session 57 read that as a paint defect TWICE before a shot taken
rem   after `fg` showed a perfectly painted window.
"%BM%\rigshot.exe" fg "%G%" >nul 2>&1
ping -n 3 127.0.0.1 >nul
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\cmp_%G%_ours.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
copy /y "%RES%\rigshot.txt" "%RES%\cmp_%G%_ours.txt" >nul 2>&1
type "%RES%\cmp_%G%_ours.txt" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\cmp_%G%_ours.log" >nul 2>&1

rem ---- 2. STOCK ------------------------------------------------------------
call :stop
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo   [IFEO Debugger REMOVED for the stock half] >> "%LOG%"
start "" C:\WIN16\%G%.EXE
ping -n 16 127.0.0.1 >nul
"%BM%\rigshot.exe" fg "%G%" >nul 2>&1
ping -n 3 127.0.0.1 >nul
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\cmp_%G%_stock.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
copy /y "%RES%\rigshot.txt" "%RES%\cmp_%G%_stock.txt" >nul 2>&1
type "%RES%\cmp_%G%_stock.txt" >> "%LOG%" 2>&1

rem ---- 3. RESTORE, EVERY TIME, AND SAY SO ----------------------------------
call :stop
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
reg query "%IFEO%" /v Debugger | find "ntvdmhost" >nul
if errorlevel 1 (echo   !! IFEO RESTORE FAILED -- STOP, everything after this measures stock >> "%LOG%") else (echo   IFEO restored >> "%LOG%")
goto :eof

:stop
"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
goto :eof
