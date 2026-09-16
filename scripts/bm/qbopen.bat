@echo off
rem qbopen.bat -- headless QBasic run that opens File > Open and walks its lists
rem (s72: "lots of problems, mostly around the file system"). Same shape as
rem qbclick.bat; every INT 21h call is traced (dostrace.flag) and the host
rem screenshots itself about once a second (capture.flag = 250 present ticks of
rem ~15 ms) so the dialog's contents can be read off the share.
rem Optional %1 = a tag: bm\qbkeys_<tag>.txt supplies the key script (a key script
rem does not fit controld's 192-byte command buffer) and the results are
rem result_qbopen<tag>.log + shot_qbopen<tag>_*.bmp. No tag = the default walk.
rem Cleans up the flags afterwards so a later by-hand run is not fed scripted input.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set GDIR=%SH%\demo\msdos\qb45
set EXE=QB.EXE
set T=qbopen%~1
if not exist "%GDIR%\%EXE%" goto nosuch
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
del /q "%OUT%\shot??.bmp" >nul 2>&1
del /q "%OUT%\shot??.txt" >nul 2>&1
> "%CFG%\target.txt" echo "%GDIR%\%EXE%"
> "%CFG%\qimode.txt" echo 20
> "%CFG%\dostrace.flag" echo.
> "%CFG%\textdump.flag" echo.
> "%CFG%\capture.flag" echo 150
rem Default: Esc (welcome box); Alt+F, O (Open dialog: *.BAS + dirs + drives);
rem Tab x3 across File Name / Files / Dirs+Drives / OK; Esc; Alt+F, X (exit).
if exist "%RIG%\qbkeys_%~1.txt" (
  copy /y "%RIG%\qbkeys_%~1.txt" "%CFG%\keys.txt" >nul
) else (
  > "%CFG%\keys.txt" echo w5000 01 w1500 d38 21 u38 w1000 18 w5000 0f w1500 0f w1500 0f w1500 01 w1500 d38 21 u38 w1000 2d w3000
)
> "%CFG%\autoexit" echo.
cd /d "%GDIR%"
start /wait "" "%RIG%\dosstub.com"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_%T%.log" >nul 2>&1
for %%f in ("%OUT%\shot??.bmp" "%OUT%\shot??.txt") do copy /y "%%f" "%OUT%\shot_%T%_%%~nxf" >nul 2>&1
del /q "%CFG%\autoexit" >nul 2>&1
del /q "%CFG%\keys.txt" >nul 2>&1
del /q "%CFG%\qimode.txt" >nul 2>&1
del /q "%CFG%\dostrace.flag" >nul 2>&1
del /q "%CFG%\textdump.flag" >nul 2>&1
del /q "%CFG%\capture.flag" >nul 2>&1
> "%OUT%\qbopen_done.txt" echo done
goto :eof
:nosuch
> "%OUT%\result_%T%.log" echo NO SUCH TARGET: %GDIR%\%EXE%
> "%OUT%\qbopen_done.txt" echo nosuch
