@echo off
rem playtest.bat -- bring up Minesweeper AND Solitaire together and LEAVE THEM UP
rem for a human to play. GH #128, session 50.
rem
rem ⚠ ONLY THE FIRST LAUNCH GOES THROUGH wowlive.bat. That script's first act is
rem   `taskkill ntvdmhost.exe`, because it exists to guarantee a FRESH host --
rem   so calling it twice would kill the first guest to start the second.
rem
rem ⚠⚠ AND target.txt MUST BE REWRITTEN BETWEEN THE TWO. This is the whole reason
rem   the first cut of this script produced TWO MINESWEEPERS and no Solitaire.
rem   `C:\ntvdmex\target.txt` is an UNCONDITIONAL OVERRIDE read at host startup
rem   (src/host/main.c, STAGE2): on a WOW launch it is where the host gets the
rem   name of the Win16 program, because Windows does not put it on the VDM's
rem   command line. So every Win16 launch on this box runs whatever that file
rem   says, no matter what was actually double-clicked -- `start SOL.EXE` with
rem   target.txt still naming WINMINE starts a second Minesweeper, cheerfully.
rem   Each guest therefore needs its own host, started AFTER target.txt names it.
rem ⚠ Which also means the two guests are two PROCESSES, not two tasks in one
rem   host: XP's shared-WOW-VDM behaviour does not apply, because each launch
rem   goes ntvdm.exe -> IFEO -> a fresh ntvdmhost.exe. Multi-task scheduling
rem   inside ONE host (wowsched.h) is exercised by WOWEXEC, not by this script.
rem ⚠ winmine.ini IS DELETED FIRST. Before session 50 this host answered
rem   GetPrivateProfileInt with 0, so Minesweeper could have SAVED Xpos=-2,
rem   Ypos=-48 on a previous exit -- and it would faithfully restore a window
rem   whose caption is off the top of the screen. Deleting it guarantees the
rem   default position for someone about to sit in front of it.
rem ⚠ IT KILLS NOTHING AT THE END. Stop them from their own windows, the tray
rem   icon, or `kill` through controld.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm

del /q "%RES%\playtest.txt" "%RES%\playtest.bmp" >nul 2>&1
del /q C:\WINDOWS\winmine.ini >nul 2>&1

call "%RES%\wowlive.bat" C:\WIN16\WINMINE.EXE
ping -n 4 127.0.0.1 >nul

tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (
    echo [playtest] !! OUR HOST IS NOT RUNNING after the first launch. > "%RES%\playtest.txt"
    echo [playtest] !! Nothing below is worth reading -- see wowlive.txt. >> "%RES%\playtest.txt"
    goto :shot
)
echo [playtest] host is up after WINMINE > "%RES%\playtest.txt"

rem -- Point target.txt at the SECOND guest before launching it, or the new host
rem    reads WINMINE again. The first host has already read the file and does not
rem    re-read it, so rewriting it now cannot disturb the guest already running.
echo C:\WIN16\SOL.EXE> C:\ntvdmex\target.txt
echo [playtest] target.txt now names SOL.EXE >> "%RES%\playtest.txt"
type C:\ntvdmex\target.txt >> "%RES%\playtest.txt"

start "" C:\WIN16\SOL.EXE
ping -n 20 127.0.0.1 >nul

tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (
    echo [playtest] !! THE HOST DIED launching the second guest. >> "%RES%\playtest.txt"
) else (
    echo [playtest] host still up with BOTH guests launched >> "%RES%\playtest.txt"
)

:shot
rem -- Nudge them apart so neither is hidden behind the other for the screenshot.
"%BM%\rigshot.exe" fg "Solitaire"
ping -n 2 127.0.0.1 >nul
"%BM%\rigshot.exe" shot "%RES%\playtest.bmp"
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\playtest_host.txt" >nul 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%RES%\playtest.txt" 2>&1
echo [playtest] STILL RUNNING -- play them; stop from their own windows. >> "%RES%\playtest.txt"
