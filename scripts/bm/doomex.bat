@echo off
rem doomex.bat -- INTERACTIVE Doom, the sibling of skyex.bat. doomrun.bat is the
rem HEADLESS acceptance test and sets the autoexit marker, which self-exits the
rem host mid-play; this one deliberately clears it and leaves the window up.
set R=C:\Documents and Settings\All Users\Documents\ntvdmex
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
ping -n 3 127.0.0.1 >nul
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%R%\bm\ntvdmhost.exe" C:\ntvdmex\ >nul
copy /y "%R%\bm\dosstub.com" C:\DOOMS\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo C:\DOOMS\DOOM.EXE> C:\ntvdmex\target.txt
del /q C:\ntvdmex\autoexit >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
cd /d C:\DOOMS
start "" C:\DOOMS\dosstub.com
