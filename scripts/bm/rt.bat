@echo off
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
rem -- STOCK NTVDM DISPATCH (GH #26). "stock <target>" in cmd.txt runs the target
rem    under stock ntvdm instead of NTVDMEX, so the oracle can be driven remotely
rem    like every other test rather than typed at the box by hand.
if /i "%1"=="stock" goto stockrun
rem -- DOOM DISPATCH. doomrun.bat lives on the share root and runs C:\DOOMS\DOOM.EXE,
rem    which no `%RES%\<name>\` game directory describes, so it needs its own arm to be
rem    drivable from cmd.txt like everything else. `doom [EXE] [DIR]`.
if /i "%1"=="doom" goto doomrun
rem -- REBOOT DISPATCH. controld.exe is the normal remote-reboot lever, but it is a
rem    SINGLE POINT OF FAILURE: runwatch.bat hot-swaps it from controld_v2.exe on every
rem    boot, so a bad staged build would overwrite the good one and leave no way to
rem    restart the box remotely. The watcher is an INDEPENDENT channel, so give it a
rem    reboot arm too -- then either channel can recover the other.
if /i "%1"=="reboot" goto rebootnow
goto normal
:rebootnow
shutdown.exe -r -f -t 03
goto :eof
:stockrun
call "%BM%\rt_stock.bat" %2
goto :eof
:doomrun
call "C:\Documents and Settings\All Users\Documents\ntvdmex\doomrun.bat" %2 %3
goto :eof
:normal
set T=%1
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
start "" "%BM%\controld.exe"
copy /y "%BM%\runwatch.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
del /q C:\ntvdmex\shot*.bmp >nul 2>&1
del /q "%RES%\shot_%T%_*.bmp" >nul 2>&1
rem ⚠⚠ AND THE HOST LOG, BECAUSE THE HOST APPENDS TO IT. (session 56)
rem    ntvdmhost.log is not truncated at startup, so result_%T%.log is WHATEVER
rem    RAN LAST followed by this run. Measured on doomrun.bat, which had the same
rem    hole: a regression taken after a live CALC session came back at twice the
rem    baseline size, and the FP counts and PM vector table read out of it were
rem    CALC's -- in a log for a guest that never loads WIN87EM. Every runner that
rem    starts a FRESH host has to do this; the interaction scripts (pbmin,
rem    minetest, wowkeys) must NOT, because they attach to a host that is already
rem    running and holding the file open.
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
if exist "%RES%\%T%\*" goto game
if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\%T%" C:\test\ >nul
echo C:\test\%T%> C:\ntvdmex\target.txt
echo.> C:\ntvdmex\autoexit
cd /d C:\test
start /wait "" C:\test\dosstub.com
goto collect
:game
rmdir /s /q C:\game >nul 2>&1
xcopy /e /i /y "%RES%\%T%" C:\game >nul
copy /y "%BM%\dosstub.com" C:\game\ >nul
echo C:\game\%T%.EXE> C:\ntvdmex\target.txt
echo.> C:\ntvdmex\autoexit
cd /d C:\game
start /wait "" C:\game\dosstub.com
:collect
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\result_%T%.log" >nul 2>&1
for %%f in (C:\ntvdmex\shot*.bmp) do copy /y "%%f" "%RES%\shot_%T%_%%~nxf" >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1
