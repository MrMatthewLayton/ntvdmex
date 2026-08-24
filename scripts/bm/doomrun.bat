@echo off
rem doomrun.bat -- THE DOS/4GW ACCEPTANCE TEST. Run Doom through NTVDMEX.
rem
rem   doomrun.bat              -> C:\DOOMS\DOOM.EXE
rem   doomrun.bat DOOM1.EXE    -> C:\DOOMS\DOOM1.EXE
rem   doomrun.bat DOOM.EXE D:\ -> a different directory
rem
rem Doom is a DOS/4GW extender: INT 2Fh 1687h to find a DPMI host, then 32-bit
rem PAGED protected mode and an LE image. It is the hardest thing this host has been
rem pointed at, and bare metal is the only place the question can even be asked --
rem QEMU+HVF aborts on DOS/4GW paged PM even under STOCK ntvdm.
rem
rem EXPECT IT TO STOP SOMEWHERE. The result is not "did it run", it is WHERE it
rem stopped: the log ends with the loud-failure block naming what was unimplemented,
rem and that list is the to-do.
setlocal
set SH=%~dp0
set EXE=%1
if "%EXE%"=="" set EXE=DOOM.EXE
set G=%2
if "%G%"=="" set G=C:\DOOMS
set N=C:\ntvdmex

echo Looking for "%G%\%EXE%" ...
if not exist "%G%\%EXE%" goto missing
echo Found it.

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist %N% md %N%
copy /y "%SH%bm\ntvdmhost.exe" %N%\ >nul
copy /y "%SH%bm\dosstub.com" "%G%\" >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
rem -- optional extra command line for the game, from doomargs.txt on the share
rem    (e.g. "-nosound"). Absent file = no arguments, exactly as before.
set ARGS=
if exist "%SH%doomargs.txt" for /f "delims=" %%a in ('type "%SH%doomargs.txt"') do set ARGS=%%a
>%N%\target.txt echo %G%\%EXE% %ARGS%
echo.> %N%\autoexit
cd /d "%G%"
start /wait /d "%G%" "" "%G%\dosstub.com"
copy /y %N%\ntvdmhost.log "%SH%result_doom.log" >nul 2>&1
rem -- COLLECT THE PCM CAPTURE TOO, AND DELETE IT FIRST.
rem    sbdump.flag makes the host write C:\ntvdmex\sb.raw, but nothing ever copied it
rem    back, so the share kept ONE capture from whenever somebody last did it by hand --
rem    and every later "before vs after" comparison silently re-analysed that same file.
rem    Two runs of a changed binary produced byte-identical histograms before this was
rem    noticed. The delete is the important half: absent is a visible failure, stale is
rem    an invisible one that reads as a result.
rem    Do NOT delete the source afterwards: the first cut did, so when the copy failed
rem    there was nothing left to diagnose with. And do not swallow the copy's output --
rem    a silent failure here is exactly what produced two "different" runs with
rem    byte-identical histograms.
del /q "%SH%doombin\sb.raw" >nul 2>&1
echo sbcopy: src=%N%\sb.raw dst=%SH%doombin\ > "%SH%sbcopy.txt"
if exist %N%\sb.raw (
  dir %N%\sb.raw >> "%SH%sbcopy.txt" 2>&1
  copy /y %N%\sb.raw "%SH%doombin\sb.raw" >> "%SH%sbcopy.txt" 2>&1
) else (
  echo sbcopy: NO SOURCE -- sbdump.flag was not set, or the host never wrote it >> "%SH%sbcopy.txt"
)
del /q %N%\autoexit >nul 2>&1
echo.
echo Done. Log copied to result_doom.log on the share.
goto :eof

:missing
rem DIAGNOSE rather than assert. "NOT FOUND, run the installer" is a guess about why;
rem the directory listing is evidence, and it also catches the case where the
rem executable is simply called something else.
echo.
echo NOT FOUND: "%G%\%EXE%"
echo Listing %G% so we can see what IS there:
echo ------------------------------------------------------------
if exist "%G%" (dir /b "%G%") else (echo   %G% does not exist at all.)
echo ------------------------------------------------------------
dir /b "%G%" > "%SH%doom_dir.txt" 2>&1
echo (also written to doom_dir.txt on the share)
rem NO `pause` HERE. This is now driven from cmd.txt by the watcher (rt.bat `doom`),
rem and a pause on the miss path would block `cmd /c rt.bat` forever -- wedging the
rem watcher and costing a reboot. The evidence is already in doom_dir.txt.
endlocal
