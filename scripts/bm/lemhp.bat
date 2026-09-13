@echo off
rem lemhp.bat -- reproduce the s69 BY-HAND crash: Lemmings, VGA + "High Performance
rem PC" (the option that CALIBRATES the PIT off 320 scanlines), driven by rigshot the
rem way a person would. s69: the user's by-hand run died right after that selection,
rem where the s68 run reached gameplay. Headless survives, so the shape matters:
rem no autoexit, no keys.txt, real window, Start-fullscreen from the registry.
rem Leaves the host running; copies the log out under a name the run cannot overwrite.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%SH%\bm
set CFG=%SH%\cfg
set OUT=%SH%\out
set R=%BM%\rigshot.exe
set TAG=%1
if "%TAG%"=="" set TAG=hp
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
rem The host APPENDS, and a scripted run would take a different path entirely.
del /q "%CFG%\autoexit" "%CFG%\keys.txt" "%CFG%\qimode.txt" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" "%OUT%\startfail.txt" >nul 2>&1
del /q "%OUT%\lemhp_%TAG%*.bmp" "%SH%\rigshot.txt" >nul 2>&1
rem livehb: a heartbeat on a LIVE run, so a host that dies leaves something that
rem says where the guest was (s68 added this flag for exactly this case).
echo.> "%CFG%\livehb.flag"
echo "%SH%\games\Lemmings\VGALEMMI.EXE"> "%CFG%\target.txt"
cd /d "%SH%\games\Lemmings"
start "" "%BM%\dosstub.com"
ping -n 10 127.0.0.1 >nul
"%R%" list >nul 2>&1
rem ⚠⚠ BRING THE GUEST FORWARD BEFORE EVERY KEY. A `controld exec` opens a visible
rem cmd.exe that STEALS THE FOREGROUND, so keys sent without this land in that console
rem and the guest never sees them -- which reads as "the run survived" while the game
rem in fact never started (io=0, no calibration). Cost a whole 8-round A/B.
rem "1" = VGA game, "2" = High Performance PCs, RETURN = confirm. THIS is the path:
rem option 1 (PC compatibles) takes a FIXED reload and never calibrates.
"%R%" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 2 127.0.0.1 >nul
"%R%" key 49 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
"%R%" key 50 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
"%R%" key 13 >nul 2>&1
ping -n 6 127.0.0.1 >nul
"%R%" shot "%OUT%\lemhp_%TAG%_1.bmp" >nul 2>&1
ping -n 6 127.0.0.1 >nul
"%R%" shot "%OUT%\lemhp_%TAG%_2.bmp" >nul 2>&1
rem Is the host still ALIVE? That is the whole measurement -- say so IN the file,
rem because "no log" and "host gone" must not look the same (s54).
echo ==== lemhp %TAG% %DATE% %TIME% ====> "%OUT%\lemhp_%TAG%.txt"
tasklist | find "ntvdmhost" >> "%OUT%\lemhp_%TAG%.txt"
if errorlevel 1 echo HOST IS GONE -- process not in tasklist>> "%OUT%\lemhp_%TAG%.txt"
type "%SH%\rigshot.txt" >> "%OUT%\lemhp_%TAG%.txt" 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\lemhp_%TAG%_host.txt" >nul 2>&1
echo done>> "%OUT%\lemhp_%TAG%.txt"
endlocal
