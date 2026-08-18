@echo off
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
if not exist C:\ntvdmex md C:\ntvdmex
if not exist C:\test md C:\test
copy /y "%BM%\rt.bat" C:\WINDOWS\ >nul
rem -- self-install to Startup so a reboot auto-recovers the watcher --
copy /y "%~f0" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1
rem -- self-upgrade the control daemon: stop any old one, pull a staged newer build, relaunch --
taskkill /f /im controld.exe >nul 2>&1
if exist "%BM%\controld_v2.exe" copy /y "%BM%\controld_v2.exe" "%BM%\controld.exe" >nul 2>&1
start "" "%BM%\controld.exe"
echo watcher up > "%SH%\watcher.txt"
title NTVDMEX test watcher -- leave this window open
echo ================================================
echo  NTVDMEX bare-metal test watcher is RUNNING.
echo  Auto-starts on reboot; control daemon launched.
echo  Leave this window open; it runs tests on demand.
echo ================================================
:loop
if exist "%SH%\cmd.txt" goto run
echo watcher up > "%SH%\watcher.txt"
ping -n 3 127.0.0.1 >nul
goto loop
:run
for /f "delims=" %%c in ('type "%SH%\cmd.txt"') do set TN=%%c
del "%SH%\cmd.txt" >nul 2>&1
echo [%TIME%] running %TN%
call C:\WINDOWS\rt.bat %TN%
echo [%TIME%] done %TN%
goto loop
