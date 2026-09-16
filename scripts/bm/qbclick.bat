@echo off
rem qbclick.bat -- headless QBasic run with a scripted mouse click (s71: the INT 33h
rem callback that never returns). Same shape as rt.bat's :run arm, but the target is
rem demos\qb45\QB.EXE and the keys script clicks once. Cleans up keys.txt/qimode.txt
rem afterwards so a later by-hand run is not fed scripted input.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set GDIR=%SH%\demo\msdos\qb45
set EXE=QB.EXE
set T=qb
if not exist "%GDIR%\%EXE%" goto nosuch
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
> "%CFG%\target.txt" echo "%GDIR%\%EXE%"
> "%CFG%\qimode.txt" echo 20
> "%CFG%\keys.txt" echo w5000 01 w1500 m0 w3000 01 w500 d38 21 u38 w1000 2d w3000
> "%CFG%\autoexit" echo.
cd /d "%GDIR%"
start /wait "" "%RIG%\dosstub.com"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_%T%.log" >nul 2>&1
del /q "%CFG%\autoexit" >nul 2>&1
del /q "%CFG%\keys.txt" >nul 2>&1
del /q "%CFG%\qimode.txt" >nul 2>&1
goto :eof
:nosuch
> "%OUT%\result_%T%.log" echo NO SUCH TARGET: %GDIR%\%EXE%
