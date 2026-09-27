@echo off
rem closeprog.bat -- File > Close Program (GH #152), driven unattended.
rem   6.22's COMMAND.COM is the shell; the key script starts a game at its prompt,
rem   the rig posts WM_COMMAND 3 (IDM_FILE_CLOSEPROG) while the game runs, and the
rem   key script then types "ver" + Enter. If the shell got control back -- with its
rem   vectors, timer and text mode intact -- VER answers at a prompt.
rem
rem   closeprog.bat doom  -> a DPMI client (DOS/4GW): the protected-mode arm
rem   closeprog.bat sky   -> Skyroads, real mode, hooks INT 08h/09h: the V86 arm
rem   closeprog.bat doomxp / skyxp -> the same under XP's own COMMAND.COM
rem
rem Results: debug\out\closeprog_<variant>.log and shot_closeprog_<variant>_*.bmp.
rem Everything this touches in cfg\ is saved first and restored after.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set RIG=%SH%\debug\rig
set V=%1
if "%V%"=="" set V=sky
set XP=0
if /i "%V%"=="doomxp" set XP=1
if /i "%V%"=="skyxp" set XP=1
set GD=%SH%\demo\msdos\skyroads
if /i "%V%"=="doom" set GD=%SH%\demo\msdos\doom
if /i "%V%"=="doomxp" set GD=%SH%\demo\msdos\doom

del /q "%OUT%\closeprog_done.txt" "%OUT%\closeprog_%V%.log" "%OUT%\ntvdmhost.log" "%OUT%\shot*.bmp" >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.closesaved" >nul
rem   *xp variants: nothing names a program, so the host loads XP's own COMMAND.COM
rem   (the product's default shell) in the launch directory.
if "%XP%"=="0" copy /y "%SH%\debug\tests\dos\COMMAND.COM" "%GD%\COMMAND.COM" >nul
if "%XP%"=="0" echo "%GD%\COMMAND.COM"> "%CFG%\target.txt"
echo.> "%CFG%\autoexit"
echo 20> "%CFG%\qimode.txt"
echo 60000> "%CFG%\headless_ms.txt"
rem   d=20 o=18 o=18 m=32 | s=1f k=25 y=15 r=13 o=18 a=1e d=20 s=1f | Enter=1c | v=2f e=12 r=13
if /i "%GD%"=="%SH%\demo\msdos\doom" (
  echo w6000 20 18 18 32 1c w30000 2f 12 13 1c w8000> "%CFG%\keys.txt"
) else (
  echo w6000 1f 25 15 13 18 1e 20 1f 1c w30000 2f 12 13 1c w8000> "%CFG%\keys.txt"
)

cd /d "%GD%"
start "" "%RIG%\dosstub.com"
rem ~24 s in: the game is running. Screenshot it, then Close Program.
ping -n 24 127.0.0.1 >nul
"%RIG%\rigshot.exe" cmd 7
ping -n 3 127.0.0.1 >nul
"%RIG%\rigshot.exe" cmd 3
rem ~45 s in: "ver" has been typed at whatever came back.
ping -n 18 127.0.0.1 >nul
"%RIG%\rigshot.exe" cmd 7
ping -n 3 127.0.0.1 >nul

copy /y "%OUT%\ntvdmhost.log" "%OUT%\closeprog_%V%.log" >nul 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_closeprog_%V%_%%~nxf" >nul 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" > "%OUT%\closeprog_done.txt" 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

if "%XP%"=="0" del /q "%GD%\COMMAND.COM" >nul 2>&1
del /q "%CFG%\target.txt" "%CFG%\autoexit" "%CFG%\qimode.txt" "%CFG%\headless_ms.txt" "%CFG%\keys.txt" >nul 2>&1
if exist "%CFG%\target.closesaved" move /y "%CFG%\target.closesaved" "%CFG%\target.txt" >nul
echo variant=%V% cfg restored>> "%OUT%\closeprog_done.txt"
endlocal
