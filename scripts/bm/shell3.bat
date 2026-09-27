@echo off
rem shell3.bat <keys> -- XP's own COMMAND.COM as the shell (nothing named), in
rem   debug\tests\dos, driven by the key script given as the arguments (scancodes,
rem   wNNNN = wait ms). Screenshots at ~20 s and ~35 s. For "does the prompt come
rem   back after a small program, twice?" (s81, the user's Doom-then-SETUP hang).
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set RIG=%SH%\debug\rig
set GD=%SH%\demo\msdos\doom
del /q "%OUT%\shell3_done.txt" "%OUT%\shell3.log" "%OUT%\ntvdmhost.log" "%OUT%\shot*.bmp" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.shell3saved" >nul
echo.> "%CFG%\autoexit"
echo 20> "%CFG%\qimode.txt"
echo 100000> "%CFG%\headless_ms.txt"
rem ⚠ controld reads at most 255 bytes of command, so a long key script cannot
rem   ride on the command line: with no arguments the script in cfg\keys.txt is used.
if not "%1"=="" echo %*> "%CFG%\keys.txt"
cd /d "%GD%"
start "" "%RIG%\dosstub.com"
ping -n 20 127.0.0.1 >nul
"%RIG%\rigshot.exe" cmd 7
ping -n 55 127.0.0.1 >nul
"%RIG%\rigshot.exe" cmd 7
ping -n 3 127.0.0.1 >nul
copy /y "%OUT%\ntvdmhost.log" "%OUT%\shell3.log" >nul 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_shell3_%%~nxf" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%CFG%\autoexit" "%CFG%\qimode.txt" "%CFG%\headless_ms.txt" "%CFG%\keys.txt" >nul 2>&1
if exist "%CFG%\target.shell3saved" move /y "%CFG%\target.shell3saved" "%CFG%\target.txt" >nul
echo done> "%OUT%\shell3_done.txt"
endlocal
