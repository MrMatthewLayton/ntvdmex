@echo off
rem doomsetup.bat -- run DOOM.EXE's SETUP.EXE interactively through NTVDMEX.
rem Sibling of doomex.bat / skyex.bat. This is the F10 acceptance case: SETUP
rem reads keys Windows would otherwise eat as MENU keys (F10, Alt+letter), so it
rem is the test for the system-key routing in wnd_proc.
rem
rem   doomsetup.bat              -> C:\DOOMS\SETUP.EXE
rem   doomsetup.bat SERSETUP.EXE -> another of the setup programs in C:\DOOMS
setlocal
set R=C:\Documents and Settings\All Users\Documents\ntvdmex
set G=C:\DOOMS
set EXE=%1
if "%EXE%"=="" set EXE=SETUP.EXE
if not exist "%G%\%EXE%" goto missing
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
ping -n 3 127.0.0.1 >nul
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%R%\bm\ntvdmhost.exe" C:\ntvdmex\ >nul
copy /y "%R%\bm\dosstub.com" "%G%\" >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo %G%\%EXE%> C:\ntvdmex\target.txt
rem INTERACTIVE: doomrun.bat sets the autoexit marker and this must NOT --
rem the host would self-exit part-way through a setup session.
del /q C:\ntvdmex\autoexit >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
cd /d "%G%"
start "" "%G%\dosstub.com"
goto :eof
:missing
echo Not found: "%G%\%EXE%"
dir /b "%G%\*.EXE"
pause
