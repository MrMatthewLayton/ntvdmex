@echo off
rem chain.bat -- EXECUTION CHAINING (north star 3): a DOS/4GW program EXEC'd from a
rem   shell, the way a user runs it. 6.22's COMMAND.COM is the guest, in Doom's own
rem   folder, and the key script types "doom" + Enter at its prompt.
rem
rem   chain.bat run   -> type doom, let it play until the headless deadline
rem   chain.bat quit  -> type doom, then F10 + y to quit it, then "ver" + Enter:
rem                      if the shell got control back, VER runs after Doom exits
rem   chain.bat twice -> doom, quit, then doom AGAIN: a second DPMI client after the
rem                      first one's teardown must start and play
rem
rem WHY THIS SHAPE. Launched directly, Doom never exits and no key reaches it before it
rem hooks INT 09h -- so the exit-with-a-parent path and the pre-hook keyboard path had
rem never run. Enter's break code is the first thing DOS/4GW's pass-up handler sees here.
rem
rem   chain.bat tsr   -> type tsr1c (t_tsr1c.com: hooks INT 1Ch and stays resident), then
rem                      doom: a DPMI client started on top of a real-mode TSR (#143)
rem   chain.bat <variant> xp -> the same, from a bare host launch, so XP's own COMMAND.COM
rem                      is the shell instead of 6.22's (6.22's hits #205 before Doom starts)
rem
rem Results: debug\out\chain_<variant>.log (the host log) and chain_done.txt.
rem Everything this touches in cfg\ is saved first and restored after.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set RIG=%SH%\debug\rig
set GD=%SH%\demo\msdos\doom
set V=%1
if "%V%"=="" set V=run

del /q "%OUT%\chain_done.txt" "%OUT%\chain_%V%.log" "%OUT%\ntvdmhost.log" "%OUT%\shot*.bmp" >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.chainsaved" >nul
copy /y "%SH%\debug\tests\dos\COMMAND.COM" "%GD%\COMMAND.COM" >nul

rem   xp: NO target.txt -- a bare host launch is what gives XP's COMMAND.COM its shell
rem   context (as a target it just runs and exits).
if /i not "%2"=="xp" echo "%GD%\COMMAND.COM"> "%CFG%\target.txt"
echo.> "%CFG%\autoexit"
echo 20> "%CFG%\qimode.txt"
echo 60000> "%CFG%\headless_ms.txt"
if /i "%V%"=="twice" echo 100000> "%CFG%\headless_ms.txt"
echo 5000> "%CFG%\capture.flag"
rem   d=20 o=18 o=18 m=32 Enter=1c | F10=44 y=15 | v=2f e=12 r=13 Enter=1c
if /i "%V%"=="tsr" copy /y "%SH%\debug\tests\dos\T_TSR1C.COM" "%GD%\TSR1C.COM" >nul
rem   t=14 s=1f r=13 1=02 c=2e Enter=1c
if /i "%V%"=="tsr" (
  echo w6000 14 1f 13 02 2e 1c w4000 20 18 18 32 1c w40000> "%CFG%\keys.txt"
) else if /i "%V%"=="twice" (
  echo w6000 20 18 18 32 1c w25000 44 w2500 15 w6000 20 18 18 32 1c w40000> "%CFG%\keys.txt"
) else if /i "%V%"=="quit" (
  echo w6000 20 18 18 32 1c w25000 44 w2500 15 w6000 2f 12 13 1c w5000> "%CFG%\keys.txt"
) else (
  echo w6000 20 18 18 32 1c w40000> "%CFG%\keys.txt"
)

cd /d "%GD%"
rem   xp: the launcher's own stub NAME (ntvdmex.com) means "the user asked for a shell";
rem   running it directly gives a real VDM that start /wait can wait on.
if /i "%2"=="xp" (
  copy /y "%RIG%\dosstub.com" "%GD%\ntvdmex.com" >nul
  start /wait "" "%GD%\ntvdmex.com"
  del /q "%GD%\ntvdmex.com" >nul 2>&1
) else (
  start /wait "" "%RIG%\dosstub.com"
)

copy /y "%OUT%\ntvdmhost.log" "%OUT%\chain_%V%.log" >nul 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_chain_%V%_%%~nxf" >nul 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" > "%OUT%\chain_done.txt" 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

del /q "%GD%\COMMAND.COM" "%GD%\TSR1C.COM" >nul 2>&1
del /q "%CFG%\target.txt" "%CFG%\autoexit" "%CFG%\qimode.txt" "%CFG%\headless_ms.txt" "%CFG%\capture.flag" "%CFG%\keys.txt" >nul 2>&1
if exist "%CFG%\target.chainsaved" move /y "%CFG%\target.chainsaved" "%CFG%\target.txt" >nul
echo variant=%V% cfg restored>> "%OUT%\chain_done.txt"
endlocal
