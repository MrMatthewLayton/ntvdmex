@echo off
rem notinst.bat -- what a FIRST-TIME user gets: open NTVDMEX before installing it.
rem   Without the IFEO key the stub would run under STOCK ntvdm and the user would get
rem   somebody else's DOS box -- which looks like it worked. The launcher must refuse.
rem   Modelled on dosstock.bat: drop the key, run, RESTORE UNCONDITIONALLY, prove it.
rem   Run with a console so install_report writes to stdout, not a modal message box.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\notinst.txt
del /q "%R%" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
echo ---- IFEO before ---- > "%R%"
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- key removed, opening NTVDMEX ---- >> "%R%"
cd /d "%SH%"
start "" cmd /c ""%BIN%\ntvdmhost.exe" > "%OUT%\notinst_stdout.txt" 2>&1"
ping -n 10 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
echo ---- what it said: ---- >> "%R%"
if exist "%OUT%\notinst_stdout.txt" type "%OUT%\notinst_stdout.txt" >> "%R%" 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
echo ---- IFEO after ---- >> "%R%"
reg query "%IFEO%" /v Debugger >> "%R%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (echo ---- restored target EXISTS ---- >> "%R%") else (echo ---- ****** RESTORED TARGET MISSING -- RIG BROKEN ****** ---- >> "%R%")
