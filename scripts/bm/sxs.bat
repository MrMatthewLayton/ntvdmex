@echo off
rem sxs.bat -- SIDE BY SIDE: a batch of Win16 guests, up under BOTH hosts at once,
rem            arranged so a human can judge them.  GH #128, session 58.
rem
rem The user's standing rule for confirming a guest, in their own words:
rem
rem   "if you want me to manually test, you need to run each app side-by-side,
rem    one in stock NTVDM and one in NTVDMEX"
rem
rem   sxs.bat                          -- the default confirmation batch below
rem   sxs.bat CALC WRITE               -- just those
rem
rem HOW IT DIFFERS FROM ITS TWO NEIGHBOURS, because three scripts that all say
rem "run it under both" is two too many unless the difference is written down:
rem   wowcompare.bat -- ONE guest, both hosts, left running.  The original.
rem   cmpsweep.bat   -- the whole shelf, both hosts, SEQUENTIALLY, killing between
rem                     each so the two halves can be PIXEL-DIFFED.  Unattended.
rem   sxs.bat (this) -- a BATCH, both hosts, ALL LEFT RUNNING AT ONCE and moved
rem                     apart, because the thing being produced is not a diff, it
rem                     is a desktop a person can walk up to and use.
rem
rem ⚠⚠ THE IFEO KEY IS THE HAZARD IN THIS FILE, as in every file that touches it.
rem   Interception IS the `Debugger` value on ntvdm.exe: present, a Win16 launch
rem   comes to us; absent, it goes to stock. So ours ALL start first with the key
rem   present, then the key is removed for the stock half, then it is restored and
rem   READ BACK. A run that left it absent would make every later test on this box
rem   silently measure stock ntvdm while producing entirely plausible logs.
rem   ⇒ A process already started is unaffected by the key, which is what makes
rem     "all of ours, then all of stock's" safe.
rem ⚠ IT KILLS NOTHING AT THE END. Both sets are left on the desktop to be used by
rem   hand -- that is the entire point. Ours stop from their tray icons, stock's by
rem   closing their windows.
rem ⚠ ASK THE HOST TO CLOSE BEFORE KILLING IT (rigshot close), or its tray icon
rem   outlives it as a ghost -- an externally terminated host never runs
rem   Shell_NotifyIcon(NIM_DELETE).  (session 56; measured 5 icons / 0 processes.)
setlocal ENABLEDELAYEDEXPANSION
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\sxs.txt
set LIST=%*
if "%LIST%"=="" set LIST=CALC WRITE CARDFILE SYSEDIT

del /q "%RES%\sxs_done.txt" >nul 2>&1
> "%LOG%" echo ==== sxs: NTVDMEX (left) vs STOCK (right), %DATE% %TIME% ====
echo list: %LIST% >> "%LOG%"

if not exist C:\ntvdmex md C:\ntvdmex

rem -- clear the decks once, then deploy once.  A host left running from an earlier
rem    invocation holds C:\ntvdmex\ntvdmhost.exe open, `copy` then fails SILENTLY,
rem    and the OLD binary runs while this script reports success.  That has happened.
"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
if errorlevel 1 echo [sxs] !! COPY FAILED -- THE OLD HOST IS ABOUT TO RUN >> "%LOG%"
copy /y "%BM%\rigshot.exe" C:\ntvdmex\ >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

rem -- 0 = a blocked GetMessage waits forever, which is what a real Win16 task does
rem    and what a session someone is going to click around in needs.
rem  !! `echo 0>file` is a redirection of handle 0, not "write 0". Parenthesise it.
(echo 0)> "%RES%\wowidle.txt"

echo. >> "%LOG%"
echo ==== 1. ALL OF OURS (IFEO Debugger present) ==== >> "%LOG%"
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
reg query "%IFEO%" /v Debugger | find "ntvdmhost" >nul
if errorlevel 1 (echo [sxs] !! NO IFEO KEY -- THIS WOULD HAVE MEASURED STOCK TWICE >> "%LOG%") else (echo [sxs] IFEO present >> "%LOG%")
for %%G in (%LIST%) do call :ours %%G

rem -- move ours out of the way BEFORE stock launches, or stock's windows appear at
rem    exactly the same coordinates and the pair is unreadable.
rem ⚠ DELETE rigshot.txt FIRST, EVERY TIME. rigshot APPENDS, so typing it without
rem   clearing it replays every earlier run's lines into this run's log -- which is
rem   exactly the "stale artefact is worse than a missing one" trap, and it cost the
rem   first invocation of this script a wrong diagnosis (four refusal message boxes
rem   from a PREVIOUS run read as this run's).
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" arrange ntvdmhost.exe left >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1

rem -- ★ AND BRING THE HOST'S OWN LOG BACK. Without it, "our guest produced no
rem    window" is an observation with no cause attached, and the cause is always in
rem    this file. (The host APPENDS to it, and :stop deleted it above, so what comes
rem    back is this run and only this run.)
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\sxs_host.txt" >nul 2>&1

echo. >> "%LOG%"
echo ==== 2. ALL OF STOCK'S (IFEO Debugger removed) ==== >> "%LOG%"
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [sxs] IFEO Debugger REMOVED >> "%LOG%"
for %%G in (%LIST%) do call :stock %%G

rem ── THE RESTORE. Unconditional, immediately, then READ BACK. Anything after this
rem    point may fail; the key is already back.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
reg query "%IFEO%" /v Debugger | find "ntvdmhost" >nul
if errorlevel 1 (echo [sxs] !! IFEO RESTORE FAILED -- STOP, everything later measures stock >> "%LOG%") else (echo [sxs] IFEO restored and read back >> "%LOG%")

del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" arrange ntvdm.exe right >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\sxs_host.txt" >nul 2>&1

echo. >> "%LOG%"
echo ==== what is on the desktop now ==== >> "%LOG%"
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdm.exe"     >> "%LOG%" 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
"%BM%\rigshot.exe" shot "%RES%\sxs.bmp" >nul 2>&1
echo [sxs] LEFT RUNNING -- ours on the LEFT half, stock on the RIGHT. >> "%LOG%"
echo DONE > "%RES%\sxs_done.txt"
goto :eof

:ours
echo C:\WIN16\%1.EXE> C:\ntvdmex\target.txt
start "" C:\WIN16\%1.EXE
ping -n 16 127.0.0.1 >nul
rem ⚠ COUNT THE HOSTS, DO NOT ASK WHETHER ONE EXISTS. `find "ntvdmhost"` answers
rem   YES as long as ANY host from an earlier guest in this batch is still up, so it
rem   reported "ours alive" for all four while three of them had exited -- a check
rem   that cannot fail is not a check. The per-guest count is the actual claim.
echo   %1: hosts running after launch: >> "%LOG%"
tasklist /fi "imagename eq ntvdmhost.exe" | find /c "ntvdmhost.exe" >> "%LOG%" 2>&1
goto :eof

:stock
start "" C:\WIN16\%1.EXE
ping -n 14 127.0.0.1 >nul
echo   %1: stock started >> "%LOG%"
goto :eof
