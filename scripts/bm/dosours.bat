@echo off
rem dosours.bat <probe> [args] -- dosstock.bat's twin for OUR host (s90): run a probe
rem from debug\tests\dos with its stdout redirected to debug\out\dosours_<probe>.txt (one file per probe, s91).
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set D=%SH%\debug\tests\dos
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\dosours_%~n1.txt
del /q "%R%" "%OUT%\dosours_done.txt" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
> "%SH%\cfg\autoexit" echo 1
cd /d "%D%"
start "" cmd /c ""%D%\%1" %2 %3 > "%R%" 2>&1"
ping -n 31 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%SH%\cfg\autoexit" >nul 2>&1
echo done > "%OUT%\dosours_done.txt"
