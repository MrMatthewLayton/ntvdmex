@echo off
rem shellpick.bat -- #203 Settings > General > DOS prompt, through the registry value it saves.
rem Five bare launches, each a new NTVDMEX with no program named:
rem   A. DosPrompt = MS-DOS 6.22's COMMAND.COM          -> that shell loads
rem   B. DosPrompt = cmd.exe (a Windows program)        -> refused, XP's own loads
rem   C. DosPrompt = a file that does not exist         -> XP's own loads, and says why
rem   D. DosPrompt = 6.22, cfg\shell.txt = XP's own     -> the file wins, log says OVERRIDDEN
rem   E. no DosPrompt                                   -> XP's own (the default)
rem Report: debug\out\shellpick.txt (+ shellpick_X.bmp). Restores the registry value and
rem the cfg files it moved.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set K=HKCU\Software\NTVDMEX
set REP=%OUT%\shellpick.txt
set S622=%SH%\debug\tests\dos\COMMAND.COM
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == shellpick %DATE% %TIME%
del /q "%OUT%\shellpick_*.bmp" "%OUT%\shellpick_*.log" >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.txt.shellpick" >nul
if exist "%CFG%\shell.txt" move /y "%CFG%\shell.txt" "%CFG%\shell.txt.shellpick" >nul
reg query "%K%" /v DosPrompt >> "%REP%" 2>&1

reg add "%K%" /v DosPrompt /t REG_SZ /d "%S622%" /f >nul
call :run A
reg add "%K%" /v DosPrompt /t REG_SZ /d "C:\WINDOWS\system32\cmd.exe" /f >nul
call :run B
reg add "%K%" /v DosPrompt /t REG_SZ /d "C:\no\such\COMMAND.COM" /f >nul
call :run C
reg add "%K%" /v DosPrompt /t REG_SZ /d "%S622%" /f >nul
> "%CFG%\shell.txt" echo C:\WINDOWS\system32\COMMAND.COM
call :run D
del /q "%CFG%\shell.txt" >nul 2>&1
reg delete "%K%" /v DosPrompt /f >nul 2>&1
call :run E

if exist "%CFG%\shell.txt.shellpick" move /y "%CFG%\shell.txt.shellpick" "%CFG%\shell.txt" >nul
if exist "%CFG%\target.txt.shellpick" move /y "%CFG%\target.txt.shellpick" "%CFG%\target.txt" >nul
>> "%REP%" echo == done %TIME%
endlocal
goto :eof

:run
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
start "" /D "%SH%" "%BIN%\ntvdmhost.exe"
ping -n 10 127.0.0.1 >nul
"%R%" shot "%OUT%\shellpick_%1.bmp" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
copy /y "%OUT%\ntvdmhost.log" "%OUT%\shellpick_%1.log" >nul 2>&1
>> "%REP%" echo -- %1
find "loading a SHELL" "%OUT%\shellpick_%1.log" >> "%REP%" 2>&1
find "DosPrompt =" "%OUT%\shellpick_%1.log" >> "%REP%" 2>&1
goto :eof
