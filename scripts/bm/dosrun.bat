@echo off
rem ============================================================================
rem  dosrun.bat -- launch ANY file the shell associates with the VDM (.EXE, .COM,
rem                .BAT, .PIF) exactly the way a double-click does, and collect
rem                the host log.
rem
rem     dosrun.bat <dir-under-demo\msdos> <file> [tag]
rem
rem  w16launch.bat does this for Win16 out of demo\win16. This is its DOS-side
rem  twin, and it exists because the interesting question for .PIF support is
rem  "what does CSRSS hand the VDM when one is double-clicked?" -- which is
rem  answered by STAGE1's own `app=[...] args=[...] pif=[...]` line and by
rem  nothing else.
rem
rem  ⚠ The target does NOT have to exist for that question to be answered: the
rem    fetch is logged before we try to open anything.
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set D=%SH%\demo\msdos
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
rem  A folder of "-" means %2 is an ABSOLUTE path, for launching something that
rem  does not live under demo\msdos -- e.g. XP's own C:\WINDOWS\SYSTEM32\COMMAND.COM.
set T=%1
set F=%2
set TAG=%3
if "%TAG%"=="" set TAG=%T%
set R=%OUT%\result_dos_%TAG%.log
if "%T%"=="-" goto :abs

if not exist "%OUT%" md "%OUT%"
> "%R%" echo == dosrun %T% %F%  %DATE% %TIME%

rem ⚠ XP's WOW/DOS VDM can be shared; kill both so the IFEO hook really fires.
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul

cd /d "%D%\%T%"
>> "%R%" echo launching %D%\%T%\%F% at %TIME%
start "" "%D%\%T%\%F%"
goto :ran

:abs
cd /d "%OUT%"
>> "%R%" echo launching ABSOLUTE %F% at %TIME%
start "" "%F%"

:ran
ping -n 13 127.0.0.1 >nul

>> "%R%" echo --- processes:
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
>> "%R%" echo --- host log (the lines that answer what we were handed):
if exist "%OUT%\ntvdmhost.log" (
  findstr /C:"STAGE1: program" /C:"STAGE1: WOW command fetch" /C:"STAGE1: CSRSS" /C:"STAGE2: loaded" /C:"STAGE2: CSRSS" /C:"STAGE2: target.txt" /C:"STAGE2: embedded fallback" /C:"app=[" "%OUT%\ntvdmhost.log" >> "%R%"
  copy /y "%OUT%\ntvdmhost.log" "%OUT%\result_dos_%TAG%_host.log" >nul 2>&1
) else (
  >> "%R%" echo *** NO HOST LOG -- stock ntvdm ran this, or nothing ran
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
>> "%R%" echo stopped at %TIME%
