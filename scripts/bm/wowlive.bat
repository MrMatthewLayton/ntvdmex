@echo off
rem wowlive.bat -- launch a Win16 program and LEAVE IT RUNNING. GH #128, session 43.
rem
rem   wowlive.bat                       -- C:\WIN16\NOTEPAD.EXE
rem   wowlive.bat C:\WIN16\PBRUSH.EXE   -- something else
rem
rem WHY THIS IS NOT wowrun.bat WITH A FLAG. wowrun.bat exists to MEASURE: it waits
rem a fixed time, screenshots, kills the host and writes wow_done.txt so the driver
rem on the other end knows the run is over. Every one of those is wrong for a
rem session someone is going to sit in front of and type into. Two files, two
rem purposes; a flag would have made both of them ambiguous.
rem
rem ⚠ IT DOES NOT KILL ANYTHING AT THE END, because there is no end. Stop it from
rem   the tray icon (Exit), by closing the guest's own window, or with
rem   `kill` through controld.
rem ⚠ AND IT NEEDS wowidle.txt TO SAY 0, or the guest quits itself after the
rem   harness's six-second message wait. The launcher writes it rather than
rem   assuming it: a run that silently used the measuring default would look like
rem   the application had crashed.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set TARGET=%~1
if "%TARGET%"=="" set TARGET=C:\WIN16\NOTEPAD.EXE

del /q "%RES%\wowlive.txt" >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex

rem -- !! KILL FIRST, THEN WAIT, THEN COPY, THEN PROVE IT. This script leaves a host
rem    RUNNING, so the next invocation finds the previous one still holding
rem    C:\ntvdmex\ntvdmhost.exe -- and `copy ... >nul` then fails SILENTLY and the
rem    OLD binary runs. That happened, and the run reported a fix that was not in
rem    the file. The wait, the errorlevel and the two directory listings are all
rem    here so it cannot happen quietly again.
rem ⚠⚠ AND KILL STOCK'S ntvdm.exe TOO -- XP's WOW VDM IS **SHARED**. If an
rem    ntvdm.exe is already running (e.g. left by wowcompare.bat), Windows hands a
rem    new Win16 launch to THAT VDM instead of creating a fresh ntvdm.exe -- so the
rem    IFEO Debugger hook never fires, our host never starts, and the program comes
rem    up under STOCK while this script cheerfully reports success. That happened,
rem    and it is what "it's only running stock NTVDM" looked like from the outside.
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ > "%RES%\wowlive.txt" 2>&1
if errorlevel 1 echo [wowlive] !! COPY FAILED -- THE OLD HOST IS ABOUT TO RUN >> "%RES%\wowlive.txt"
echo ==== deployed vs source (sizes must match) ==== >> "%RES%\wowlive.txt"
dir "%BM%\ntvdmhost.exe" C:\ntvdmex\ntvdmhost.exe >> "%RES%\wowlive.txt" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

rem -- 0 = a blocked GetMessage waits forever, which is what a real Win16 task does.
rem  !! `echo 0>file` IS NOT "write 0": `0>` is a redirection of handle 0, so the
rem    first cut wrote an EMPTY file, the host kept its 6-second default, and the
rem    guest quit while this script reported success. Parenthesise it.
(echo 0)> "%RES%\wowidle.txt"

rem ⚠ AND THE TWO SWITCHES, because a guest CANNOT launch without them and this
rem   script exists to leave one running. The baseline is measured with them OFF
rem   (`270/46/122/96`), so anything that deletes them for a baseline run leaves
rem   the box unable to start a guest — and the symptom is our host starting,
rem   stopping at `0001:229C` and exiting, which from outside looks exactly like
rem   "the guest went to stock". That is a lap I have already run.
rem   wowrun.bat deliberately does NOT do this: it is the MEASURING launcher.
if not exist "%RES%\wowsched.txt" type nul > "%RES%\wowsched.txt"
if not exist "%RES%\wowcall.txt"  type nul > "%RES%\wowcall.txt"
echo [wowlive] wowsched.txt + wowcall.txt ensured (a guest cannot launch without them) >> "%RES%\wowlive.txt"

echo %TARGET%> C:\ntvdmex\target.txt
echo [wowlive] target = %TARGET%          >> "%RES%\wowlive.txt"
echo [wowlive] wowidle.txt = 0 (no timeout) >> "%RES%\wowlive.txt"

start "" "%TARGET%"

rem -- Give it time to build its window, then say what is on the screen. This is
rem    the ONLY report; there is no completion marker because nothing completes.
ping -n 12 127.0.0.1 >nul
rem ⚠ AND SAY SO LOUDLY IF IT IS NOT OURS. `tasklist` alone prints "INFO: No tasks
rem   running" and the script then reports STILL RUNNING regardless -- another tool
rem   succeeding while doing nothing. The `find` makes the absence an ERRORLEVEL.
tasklist /fi "imagename eq ntvdmhost.exe" >> "%RES%\wowlive.txt" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (
    echo [wowlive] !! OUR HOST IS NOT RUNNING -- the guest went to STOCK ntvdm. >> "%RES%\wowlive.txt"
    echo [wowlive] !! Usual cause: a stock ntvdm.exe was already up and XP's WOW  >> "%RES%\wowlive.txt"
    echo [wowlive] !! VDM is SHARED, so the launch joined it and the IFEO hook     >> "%RES%\wowlive.txt"
    echo [wowlive] !! never fired. Kill ntvdm.exe and re-run.                      >> "%RES%\wowlive.txt"
)
"%BM%\rigshot.exe" shot "%RES%\wowlive.bmp" >> "%RES%\wowlive.txt" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\wowlive_host.txt" >nul 2>&1
echo [wowlive] STILL RUNNING -- stop it from the tray icon, the guest's own >> "%RES%\wowlive.txt"
echo [wowlive] window, or `kill` through controld.                          >> "%RES%\wowlive.txt"
