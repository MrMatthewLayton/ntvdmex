@echo off
rem zarlong.bat -- ZAR under NTVDMEX with a REAL amount of time, then a screenshot,
rem                and LEFT RUNNING for a human.  #23
rem
rem WHY THIS EXISTS. The headless runner (rt.bat) caps a run at 45 s and the compare
rem scripts screenshot after ~30 s. Both were fine while ZAR died in half a second;
rem now that it gets as far as "Game loading..." they cut it off mid-load and the
rem evidence reads as "no video mode set" when the truth is "not finished yet".
rem   `HEADLESS: deadline reached -> g_running=0 (wind down)` is the tell.
rem
rem ⚠ NO autoexit MARKER and NO `start /wait`. The marker makes the host exit with its
rem   guest, which is wrong when the point is to LOOK at the guest; /wait on a host that
rem   keeps its window open wedges the box (measured, s58). So: start, sleep, shoot,
rem   leave it up.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\zarlong.txt
set WAIT=%1
if "%WAIT%"=="" set WAIT=90

del /q "%RES%\zarlong_done.txt" "%RES%\zarlong.bmp" >nul 2>&1
> "%LOG%" echo ==== zarlong: %DATE% %TIME%  (wait %WAIT%s) ====

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1

reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo C:\game\ZAR.EXE> C:\ntvdmex\target.txt
cd /d C:\game
start "" C:\game\ZAR.EXE
ping -n %WAIT% 127.0.0.1 >nul

rem ⚠ FORCE A REPAINT BEFORE THE SHOT -- the desktop keeps stale pixels and session 57
rem   read that as a paint defect twice before shooting after `fg`.
"%BM%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\zarlong.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\zarlong_host.txt" >nul 2>&1
echo [zarlong] LEFT RUNNING -- stop it from the tray icon or its own window. >> "%LOG%"
echo DONE > "%RES%\zarlong_done.txt"
