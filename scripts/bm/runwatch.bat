@echo off
rem ============================================================================
rem  NTVDMEX test watcher -- polls the share for cmd.txt and runs rt.bat.
rem
rem  s61: THIS SCRIPT WAS ITSELF A SOURCE OF LITTER, and it put it back every time
rem  it started, so deleting the mess by hand never held:
rem       md C:\ntvdmex          <- recreated on every watcher start
rem       md C:\test             <- same
rem       copy rt.bat C:\WINDOWS <- same
rem  All three are gone.  rt.bat is now called from bm\ where it lives, and the
rem  only thing this installs outside the share is its own Startup entry, which is
rem  what lets the box recover the watcher after a reboot without a human.
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%SH%\bm

if not exist "%SH%\cfg" md "%SH%\cfg"
if not exist "%SH%\out" md "%SH%\out"

rem -- self-install to Startup so a reboot auto-recovers the watcher --
copy /y "%~f0" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1
rem -- self-upgrade the control daemon: stop any old one, pull a staged newer build, relaunch --
taskkill /f /im controld.exe >nul 2>&1
if exist "%BM%\controld_v2.exe" copy /y "%BM%\controld_v2.exe" "%BM%\controld.exe" >nul 2>&1
start "" "%BM%\controld.exe"
echo watcher up > "%SH%\watcher.txt"
title NTVDMEX test watcher -- leave this window open
echo ================================================
echo  NTVDMEX bare-metal test watcher is RUNNING.
echo  Auto-starts on reboot; control daemon launched.
echo  Leave this window open; it runs tests on demand.
echo ================================================
:loop
if exist "%SH%\cmd.txt" goto run
echo watcher up > "%SH%\watcher.txt"
ping -n 3 127.0.0.1 >nul
goto loop
:run
rem -- CLEAR TN FIRST. `for /f ... do set TN=%%c` only assigns if the read yields a
rem    line; if cmd.txt is momentarily present but EMPTY (an SMB create and write are
rem    two operations), the body never runs and TN keeps its PREVIOUS value -- so the
rem    watcher silently re-runs the last target. That cost a real debugging session:
rem    "skyroads" was queued and p_ver.com ran, and the resulting log looked plausible.
set TN=
for /f "delims=" %%c in ('type "%SH%\cmd.txt"') do set TN=%%c
if "%TN%"=="" goto emptycmd
del "%SH%\cmd.txt" >nul 2>&1
echo [%TIME%] running %TN%
rem -- ISOLATE THE TEST FROM THE LOOP. `call` runs rt.bat inside THIS cmd.exe, so
rem    anything that takes the test down takes the watcher with it -- which is what
rem    happens when NTVDMEX exits (it is linked console-subsystem, so it shares in
rem    console control events and teardown). Observed repeatedly today as "the
rem    watcher died too", costing a reboot each time. cmd /c gives the test its own
rem    process, so the loop survives whatever the test does to itself.
cmd /c ""%BM%\rt.bat" %TN%"
echo [%TIME%] done %TN%
goto loop
:emptycmd
del "%SH%\cmd.txt" >nul 2>&1
echo [%TIME%] EMPTY cmd.txt -- ignored, not re-running the last target
goto loop
