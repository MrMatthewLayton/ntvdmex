@echo off
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
taskkill /f /im ntvdmhost.exe >nul 2>&1
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
del /q C:\ntvdmex\ldtprobe.log >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
del /q C:\ntvdmex\wdprobe.log >nul 2>&1
rem -- ESTABLISH THE INPUT, DO NOT INHERIT IT. target.txt is written by rt.bat for
rem    every DOS test, and a WOW run that leaves it alone gets whatever DOS program
rem    ran last. Session 31: every WOW run was handed C:\test\selftest.com and
rem    krnl386 dutifully tried to load a .COM file as a Win16 module.
echo C:\WINDOWS\SYSTEM32\SYSEDIT.EXE> C:\ntvdmex\target.txt
rem -- (session 36) krnl386's own BOOTLOG.TXT narration, armed by wowbootlog.flag.
rem    DELETE EVERY CANDIDATE FIRST: the guest APPENDS (it seeks to end), so a stale
rem    file would carry a previous run's lines and read as this run's evidence.
del /q "C:\WINDOWS\BOOTLOG.TXT" >nul 2>&1
del /q "C:\BOOTLOG.TXT" >nul 2>&1
del /q "C:\WINDOWS\SYSTEM32\BOOTLOG.TXT" >nul 2>&1
del /q "C:\ntvdmex\BOOTLOG.TXT" >nul 2>&1
del /q "%RES%\wow_bootlog.txt" >nul 2>&1
del /q "%RES%\wow_bootlog_where.txt" >nul 2>&1
start "" C:\WINDOWS\SYSTEM32\sysedit.exe
ping -n 4 127.0.0.1 >nul
tasklist /fi "imagename eq ntvdmhost.exe" > "%RES%\wow_alive.txt" 2>&1
rem -- SEE THE SCREEN. (#128, session 42) The rig has no VNC and the host's own
rem    self-capture only runs headless, so `rigshot shot` on the DESKTOP is the only
rem    eye there is on what a Win16 run actually draws. Taken TWICE: once while the
rem    guest is still running and once at the end, because "nothing was drawn" and
rem    "it was drawn and then the window closed" are different facts.
rem    DELETE FIRST -- a stale BMP is a silent wrong answer, a missing one is loud.
del /q "%RES%\wow_shot1.bmp" >nul 2>&1
del /q "%RES%\wow_shot2.bmp" >nul 2>&1
del /q "%RES%\wow_shot.txt" >nul 2>&1
ping -n 20 127.0.0.1 >nul
"%BM%\rigshot.exe" shot "%RES%\wow_shot1.bmp" > "%RES%\wow_shot.txt" 2>&1
"%BM%\rigshot.exe" list >> "%RES%\wow_shot.txt" 2>&1
ping -n 51 127.0.0.1 >nul
"%BM%\rigshot.exe" shot "%RES%\wow_shot2.bmp" >> "%RES%\wow_shot.txt" 2>&1
echo ---- at end of run ---- >> "%RES%\wow_alive.txt"
tasklist /fi "imagename eq ntvdmhost.exe" >> "%RES%\wow_alive.txt" 2>&1
copy /y C:\ntvdmex\ldtprobe.log "%RES%\wow_ldt.txt" >nul 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\wow_host.txt" >nul 2>&1
copy /y C:\ntvdmex\wdprobe.log "%RES%\wow_wd.txt" >nul 2>&1
rem -- collect krnl386's own narration, and record WHERE it was looked for, so an
rem    absent file is distinguishable from a file we failed to find.
dir "C:\WINDOWS\BOOTLOG.TXT" "C:\BOOTLOG.TXT" "C:\WINDOWS\SYSTEM32\BOOTLOG.TXT" "C:\ntvdmex\BOOTLOG.TXT" > "%RES%\wow_bootlog_where.txt" 2>&1
copy /y "C:\WINDOWS\BOOTLOG.TXT" "%RES%\wow_bootlog.txt" >nul 2>&1
if not exist "%RES%\wow_bootlog.txt" copy /y "C:\BOOTLOG.TXT" "%RES%\wow_bootlog.txt" >nul 2>&1
if not exist "%RES%\wow_bootlog.txt" copy /y "C:\WINDOWS\SYSTEM32\BOOTLOG.TXT" "%RES%\wow_bootlog.txt" >nul 2>&1
if not exist "%RES%\wow_bootlog.txt" copy /y "C:\ntvdmex\BOOTLOG.TXT" "%RES%\wow_bootlog.txt" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
echo done > "%RES%\wow_done.txt"
