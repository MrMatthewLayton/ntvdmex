@echo off
rem shelfsweep.bat -- launch every guest on the shelf that has NEVER been run,
rem one at a time, and let each one name its own blocker. (session 55)
rem
rem ⚠ ONE AT A TIME, AND target.txt REWRITTEN BETWEEN EACH. XP's WOW VDM is
rem   SHARED and C:\ntvdmex\target.txt is an unconditional override, so two
rem   launches without a rewrite start the same program twice, cheerfully.
rem ⚠ AND THE IFEO VALUE IS RE-ADDED EVERY TIME: the taskkill below manufactures
rem   the unclean starts that GH #132's recovery counts, and at three it drops
rem   the key and the rest of the sweep silently measures stock ntvdm.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
del /q "%RES%\sweep_done.txt" >nul 2>&1
echo sweep starting > "%RES%\sweep.txt"

for %%G in (SOUNDREC PACKAGER CARDFILE WINFILE WRITE PROGMAN TERMINAL) do call :one %%G
echo DONE > "%RES%\sweep_done.txt"
goto :eof

:one
set G=%1
echo ================ %G% ================ >> "%RES%\sweep.txt"
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d C:\ntvdmex\ntvdmhost.exe /f >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
if not exist "%RES%\wowsched.txt" type nul > "%RES%\wowsched.txt"
if not exist "%RES%\wowcall.txt"  type nul > "%RES%\wowcall.txt"
(echo 0)> "%RES%\wowidle.txt"
echo C:\WIN16\%G%.EXE> C:\ntvdmex\target.txt
start "" C:\WIN16\%G%.EXE
ping -n 16 127.0.0.1 >nul
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >nul
if errorlevel 1 (echo   !! HOST NOT RUNNING -- went to stock, or exited >> "%RES%\sweep.txt") else (echo   host alive >> "%RES%\sweep.txt")
"%BM%\rigshot.exe" shot "%RES%\sweep_%G%.bmp" >nul 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%RES%\sweep.txt" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\sweep_%G%.log" >nul 2>&1
goto :eof
