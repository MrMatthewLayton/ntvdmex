@echo off
rem ============================================================================
rem  NTVDMEX bare-metal runner -- ONE FOLDER, NOTHING ON C:.  (s61 rewrite, s73 layout)
rem
rem  The previous version spread the rig over five places and the user cleared the
rem  box because of it.  What it used to do, and no longer does:
rem     md C:\ntvdmex          host + log + target.txt + autoexit + shots
rem     md C:\test             a copy of each test program
rem     rmdir/xcopy C:\game    a FULL COPY of the game, every single run
rem     copy rt.bat C:\WINDOWS
rem     result_*.log + shot_*.bmp dropped in the share root
rem
rem  THE s73 LAYOUT (the share root is also what gets copied to a USB for a release):
rem     bin\          the working host binary -- the ONLY thing in here
rem     dist\         the installable zips
rem     cfg\          everything the host READS: knobs, flags, target.txt, FLOPPY.IMG
rem     debug\rig\    this harness: rt.bat, runwatch.bat, controld, rigshot, vdmwatch
rem     debug\tests\  DOS test programs: Probe\ Argtest\ Testcard\ dos\ selftest\
rem     debug\out\    everything the host WRITES: ntvdmhost.log, result_*.log, shots
rem     debug\prev\   rollback host builds (never run from here)
rem     demo\msdos\   THE USER'S games and demos -- run IN PLACE, never copied
rem     demo\win16\   Win 3.11 apps
rem  C: gets nothing at all.  bin\ bm\-compat: the host also accepts a folder named bm.
rem
rem  USAGE (via cmd.txt, one line):
rem     <Name> [EXE]   run demo\msdos\<Name>\<EXE>, else debug\tests\<Name>\<EXE>
rem                    (EXE defaults to <Name>.EXE)
rem     setup          install watcher + IFEO, create cfg\ and debug\out\
rem     clean          remove every artefact the OLD layouts left behind
rem     reboot         restart the box
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set TESTS=%SH%\debug\tests
set DEMO=%SH%\demo\msdos
set CFG=%SH%\cfg
set OUT=%SH%\debug\out

if /i "%1"=="reboot" goto rebootnow
if /i "%1"=="clean"  goto cleanup
if /i "%1"=="setup"  goto setup
if /i "%1"=="warm"   goto warmup
if /i "%1"=="noprefetch" goto noprefetch
if /i "%1"=="live"   goto live
if /i "%1"=="stop"   goto stop
if "%1"==""          goto nogame
goto run

rem ---------------------------------------------------------------------------
rem  LIVE -- launch a game for a HUMAN to play, and return immediately.
rem
rem  Differs from :run in the two things that matter for a person at the box:
rem    * NO cfg\autoexit.  That marker is what turns on headless mode, and headless
rem      starts the deadline thread that clears g_running after ~30 s plus the
rem      real-mode and PM step caps.  A game being played must not be on a timer.
rem    * NO `start /wait`.  The watcher must come straight back so it can still be
rem      told `stop`, or queue the next game, while this one is on screen.
rem
rem  USAGE:  live <GameDir> [EXE]
rem ---------------------------------------------------------------------------
:live
set T=%2
set EXE=%3
if "%EXE%"=="" set EXE=%T%.EXE
set GDIR=%DEMO%\%T%
if not exist "%GDIR%\%EXE%" set GDIR=%TESTS%\%T%
if not exist "%CFG%" md "%CFG%"
if not exist "%SH%\debug" md "%SH%\debug"
if not exist "%OUT%" md "%OUT%"
if not exist "%GDIR%\%EXE%" goto livenosuch

taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
rem ⚠ THE MARKER MUST BE GONE, not merely unwritten: a headless run that was killed
rem    leaves it behind, and the next live game would then quietly die on the cap.
del /q "%CFG%\autoexit" >nul 2>&1
rem ⚠⚠ AND SO MUST THE START-FAILURE COUNTER (GH #132). It is raised on every start
rem    and cleared ONLY by a clean exit -- so every host this harness taskkills, and
rem    every window-close that does not reach the clean-exit path, leaves it up. At
rem    three it makes the host UNINSTALL ITS OWN IFEO KEY and quit in ~170 ms with no
rem    log, which reads exactly like "the host is broken" and silently hands the box
rem    back to stock ntvdm. That is what "it booted in stock NTVDM" was.
rem    The counter defends a USER against a host that wedges on startup; on a rig that
rem    kills hosts deliberately it only ever fires as a false positive.
del /q "%OUT%\startfail.txt" >nul 2>&1
rem ARGS: everything after the EXE is passed to the guest. Needed by testcard.com,
rem which takes a digit to HOLD one video mode -- a card that only appears for 2.5s
rem in a cycle cannot be caught reliably by a periodic screenshot.
echo "%GDIR%\%EXE%" %3 %4 %5> "%CFG%\target.txt"

rem A header in the notes file so the report is per-game without anyone typing one.
echo.>> "%OUT%\notes.txt"
echo ==== %T% (%EXE%)  %DATE% %TIME% ====>> "%OUT%\notes.txt"

rem ⚠⚠ /wait, AND IT IS NOT OPTIONAL. Without it this cmd exits as soon as `start`
rem    returns, and OUR HOST IS CONSOLE-SUBSYSTEM -- it shares in the parent console's
rem    teardown and dies with it. Measured: a `live` launch reported "launched" and
rem    produced NO LOG AT ALL, because rt.bat had already gone. The headless :run arm
rem    only ever worked because it used /wait.
rem  ► The cost is that the watcher BLOCKS until the game is quit, so `stop` cannot be
rem    queued while one is up. That is the right trade: a game you can actually play
rem    beats a queue slot, and closing the window ends it.
echo launched %GDIR%\%EXE% at %TIME% > "%OUT%\result_live.log"
cd /d "%GDIR%"
start /wait "" "%RIG%\dosstub.com"
echo exited at %TIME% >> "%OUT%\result_live.log"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_%T%.log" >nul 2>&1
goto :eof

:livenosuch
echo NO SUCH TARGET: %GDIR%\%EXE% > "%OUT%\result_live.log"
echo --- what is in %GDIR%: >> "%OUT%\result_live.log"
dir /b "%GDIR%" >> "%OUT%\result_live.log" 2>&1
goto :eof

rem ---------------------------------------------------------------------------
rem  STOP -- take down whatever is on screen and keep its log.
rem ---------------------------------------------------------------------------
:stop
if not exist "%OUT%" md "%OUT%"
taskkill /f /im ntvdmhost.exe >nul 2>&1
if not "%2"=="" copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_%2.log" >nul 2>&1
echo stopped at %TIME% > "%OUT%\result_stop.log"
goto :eof

:rebootnow
shutdown.exe -r -f -t 03
goto :eof

rem ---------------------------------------------------------------------------
rem  SETUP -- everything the rig needs to exist, and nothing more.
rem  Idempotent: safe to run any number of times.
rem ---------------------------------------------------------------------------
rem ---------------------------------------------------------------------------
rem  NOPREFETCH -- turn XP's application prefetcher OFF.  (s64)
rem
rem  WHY THIS EXISTS, AND IT IS NOT A MICRO-OPTIMISATION.  XP TRACES THE FIRST ~10
rem  SECONDS of a new process and then writes a .pf file for it.  The trace is keyed
rem  to the executable, so DEPLOYING A NEW BINARY INVALIDATES IT and the very next
rem  launch re-traces -- background work, on a 2-core box, during exactly the window
rem  where Skyroads is establishing its frame pacing.
rem
rem  That is the shape the user reported on 2026-09-10: "the first run after a build
rem  drops frames, every run after that is fine".  It has almost certainly been read
rem  as a TIMING REGRESSION IN OUR CODE more than once, and re-fixed accordingly.
rem  A test rig has no use for a desktop-responsiveness optimisation that perturbs
rem  the thing under measurement -- so remove the variable rather than work around it.
rem
rem  0 = off, 1 = applications, 2 = boot, 3 = both (XP default).  NEEDS A REBOOT.
rem ---------------------------------------------------------------------------
:noprefetch
if not exist "%OUT%" md "%OUT%"
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters" /v EnablePrefetcher /t REG_DWORD /d 0 /f >nul
echo EnablePrefetcher set to 0 at %TIME% -- REBOOT REQUIRED > "%OUT%\result_noprefetch.log"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management\PrefetchParameters" /v EnablePrefetcher >> "%OUT%\result_noprefetch.log" 2>&1
echo --- existing traces (deleted so nothing stale is reused): >> "%OUT%\result_noprefetch.log"
dir /b "%SystemRoot%\Prefetch\*.pf" >> "%OUT%\result_noprefetch.log" 2>&1
del /q "%SystemRoot%\Prefetch\*.pf" >nul 2>&1
goto :eof

rem ---------------------------------------------------------------------------
rem  WARM -- read the host binary into the file cache after a deploy, so the first
rem  REAL run is not the one that pays for faulting a freshly-written 1.6 MB PE in.
rem  Belt and braces alongside :noprefetch; costs about a second.
rem ---------------------------------------------------------------------------
:warmup
if not exist "%OUT%" md "%OUT%"
type "%BIN%\ntvdmhost.exe" > nul 2>&1
echo warmed %TIME% > "%OUT%\result_warm.log"
dir "%BIN%\ntvdmhost.exe" >> "%OUT%\result_warm.log" 2>&1
goto :eof

:setup
if not exist "%CFG%" md "%CFG%"
if not exist "%SH%\debug" md "%SH%\debug"
if not exist "%OUT%" md "%OUT%"
rem The IFEO Debugger key is the whole interception mechanism.  It must point at a
rem binary that EXISTS -- if it does not, EVERY 16-bit launch on the box fails,
rem which is exactly what "nothing ran at all" looked like after the wipe.
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
rem Watcher auto-recovery on reboot.  One file in Startup; it is how the box comes
rem back without someone standing at it.
copy /y "%RIG%\runwatch.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1
echo setup done > "%OUT%\result_setup.log"
echo IFEO: >> "%OUT%\result_setup.log"
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger >> "%OUT%\result_setup.log" 2>&1
echo host: >> "%OUT%\result_setup.log"
dir "%BIN%\ntvdmhost.exe" >> "%OUT%\result_setup.log" 2>&1
echo demo\msdos: >> "%OUT%\result_setup.log"
dir /b "%DEMO%" >> "%OUT%\result_setup.log" 2>&1
echo debug\tests: >> "%OUT%\result_setup.log"
dir /b "%TESTS%" >> "%OUT%\result_setup.log" 2>&1
goto :eof

rem ---------------------------------------------------------------------------
rem  CLEAN -- undo the OLD layout.  Reports what it found so the result log is
rem  evidence the box is actually clean, not just a claim that it is.
rem ---------------------------------------------------------------------------
:cleanup
if not exist "%OUT%" md "%OUT%"
echo === BEFORE === > "%OUT%\result_clean.log"
if exist C:\ntvdmex      echo FOUND C:\ntvdmex      >> "%OUT%\result_clean.log"
if exist C:\test         echo FOUND C:\test         >> "%OUT%\result_clean.log"
if exist C:\game         echo FOUND C:\game         >> "%OUT%\result_clean.log"
if exist C:\DOOMS        echo FOUND C:\DOOMS        >> "%OUT%\result_clean.log"
if exist C:\WINDOWS\rt.bat echo FOUND C:\WINDOWS\rt.bat >> "%OUT%\result_clean.log"
taskkill /f /im ntvdmhost.exe >nul 2>&1
rmdir /s /q C:\ntvdmex >nul 2>&1
rmdir /s /q C:\test    >nul 2>&1
rmdir /s /q C:\game    >nul 2>&1
del /q C:\WINDOWS\rt.bat >nul 2>&1
rem Share-root litter from the old layout (knobs now live in cfg\, results in out\).
rem ⚠ NAMED, NOT GLOBBED. `del %SH%\*.txt` would take watcher.txt, controld.txt and
rem    cmd.txt with it -- the watcher's own control channel -- and the watcher would
rem    go deaf mid-clean with no way to tell it anything.
del /q "%SH%\*.flag"          >nul 2>&1
del /q "%SH%\*.bmp"           >nul 2>&1
del /q "%SH%\result_*.log"    >nul 2>&1
del /q "%SH%\doomrun.bat"     >nul 2>&1
del /q "%SH%\ntvdmhost.log"   >nul 2>&1
del /q "%SH%\notes.txt"       >nul 2>&1
rem The s61 share layout (bm\ games\ demos\ out\) that s73 folded into bin\ debug\
rem demo\. Only removed once EMPTY -- this never deletes a game or a log by itself.
rd "%SH%\bm"    >nul 2>&1
rd "%SH%\games" >nul 2>&1
rd "%SH%\demos" >nul 2>&1
rd "%SH%\out"   >nul 2>&1
echo === AFTER === >> "%OUT%\result_clean.log"
if exist C:\ntvdmex      echo STILL C:\ntvdmex      >> "%OUT%\result_clean.log"
if exist C:\test         echo STILL C:\test         >> "%OUT%\result_clean.log"
if exist C:\game         echo STILL C:\game         >> "%OUT%\result_clean.log"
if exist C:\WINDOWS\rt.bat echo STILL C:\WINDOWS\rt.bat >> "%OUT%\result_clean.log"
echo --- C:\ root --- >> "%OUT%\result_clean.log"
dir /b C:\ >> "%OUT%\result_clean.log" 2>&1
echo --- share root --- >> "%OUT%\result_clean.log"
dir /b "%SH%" >> "%OUT%\result_clean.log" 2>&1
goto :eof

:nogame
if not exist "%OUT%" md "%OUT%"
echo no target given > "%OUT%\result_none.log"
goto :eof

rem ---------------------------------------------------------------------------
rem  RUN -- demo\msdos\<Name>\<EXE> (or debug\tests\<Name>\<EXE>), IN PLACE.
rem ---------------------------------------------------------------------------
:run
set T=%1
set EXE=%2
if "%EXE%"=="" set EXE=%T%.EXE
rem A target is a game or demo first, a test program second -- same name, same run.
set GDIR=%DEMO%\%T%
if not exist "%GDIR%\%EXE%" set GDIR=%TESTS%\%T%

if not exist "%CFG%" md "%CFG%"
if not exist "%SH%\debug" md "%SH%\debug"
if not exist "%OUT%" md "%OUT%"
if not exist "%GDIR%\%EXE%" goto nosuch

taskkill /f /im ntvdmhost.exe >nul 2>&1
rem The host APPENDS to its log, so a stale one makes result_<T>.log whatever ran
rem last followed by this run -- that cost a whole session's wrong conclusions.
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\shot*.bmp" >nul 2>&1
rem GH #132: a taskkill above is an unclean exit; the counter must not carry over.
del /q "%OUT%\startfail.txt" >nul 2>&1

rem QUOTED: the share path contains "Documents and Settings", and an unquoted path
rem with a space used to be split into program + arguments at the first one.
echo "%GDIR%\%EXE%" %3 %4 %5> "%CFG%\target.txt"
echo.> "%CFG%\autoexit"

rem Run FROM the game's own directory so it finds its data files, and so anything
rem it writes (config, saves) lands where a real install would put them.
cd /d "%GDIR%"
start /wait "" "%RIG%\dosstub.com"

copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_%T%.log" >nul 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_%T%_%%~nxf" >nul 2>&1
del /q "%CFG%\autoexit" >nul 2>&1
goto :eof

:nosuch
echo NO SUCH TARGET: %GDIR%\%EXE% > "%OUT%\result_%T%.log"
echo --- what is in %GDIR%: >> "%OUT%\result_%T%.log"
dir /b "%GDIR%" >> "%OUT%\result_%T%.log" 2>&1
goto :eof
