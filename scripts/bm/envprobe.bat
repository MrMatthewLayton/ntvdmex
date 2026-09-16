@echo off
rem envprobe.bat -- what environment does a DOS program get?  (s73)
rem   envprobe.bat stock | host
rem Runs debug\tests\dos\p_env.com from THIS cmd line with a redirect, the GH #131
rem shape, under stock ntvdm (the oracle for "what XP hands a DOS program") or under
rem NTVDMEX. Two variables are set here so their fate can be read off the dump: LIB
rem (what LINK reads) with a long path, and a lower-case one (stock upper-cases the
rem NAME, or does it?). Report: debug\out\envprobe_<who>.txt; envprobe_done.txt.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set TESTS=%SH%\debug\tests\dos
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set WHO=%1
if "%WHO%"=="" set WHO=host
set R=%OUT%\envprobe_%WHO%.txt
del /q "%OUT%\envprobe_done.txt" >nul 2>&1
> "%R%" echo == envprobe %WHO%  %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
del /q "%OUT%\startfail.txt" >nul 2>&1
if "%WHO%"=="stock" (
  reg delete "%IFEO%" /v Debugger /f >nul 2>&1
) else (
  reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
  del /q "%OUT%\ntvdmhost.log" >nul 2>&1
  > "%CFG%\autoexit" echo.
)
set LIB=%SH%\demo\msdos\qb45\LIB
set lowercase_name=kept as typed
set NTVDMEX_PROBE=%WHO%
rem %2 = a TEMP/TMP override, to measure what stock does with them (it gave
rem C:\WINDOWS\TEMP for a user temp that exists in 8.3 form already).
if not "%2"=="" set TEMP=%2
if not "%2"=="" set TMP=%2
if not "%2"=="" set R=%OUT%\envprobe_%WHO%_%3.txt
cd /d "%TESTS%"
p_env.com > "%OUT%\envprobe_out_%WHO%.txt" 2>&1
>> "%R%" echo rc=%ERRORLEVEL%
type "%OUT%\envprobe_out_%WHO%.txt" >> "%R%"
if "%WHO%"=="stock" (
  reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
  >> "%R%" echo [stock] IFEO Debugger restored
) else (
  del /q "%CFG%\autoexit" >nul 2>&1
  copy /y "%OUT%\ntvdmhost.log" "%OUT%\envprobe_host.log" >nul 2>&1
  >> "%R%" echo -- host log:
  findstr /c:"STAGE1: command fetch" /c:"guest environment" /c:"STAGE2: stdout" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
)
>> "%R%" echo == done %TIME%
> "%OUT%\envprobe_done.txt" echo done
