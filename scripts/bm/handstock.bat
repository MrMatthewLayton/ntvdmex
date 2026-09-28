@echo off
rem handstock.bat <demo-subpath> <EXE> [<demo-subpath> <EXE>] -- start programs under STOCK
rem   ntvdm FOR A PERSON to compare by hand, and put NTVDMEX's routing straight back. (s83)
rem
rem   handstock.bat win16\pbrush PBRUSH.EXE msdos\mario MARIO.EXE
rem
rem WHY NOT w16stock.bat / dosstock.bat: those are for probes -- they taskkill every host
rem first (a user's own session included) and kill stock after 16 s. Here nothing is killed:
rem the IFEO Debugger value is removed only for the ~10 s it takes the programs to START
rem (IFEO is read at process creation, so a started stock ntvdm keeps running), then
rem restored and verified, so every later launch is NTVDMEX again.
rem ⚠ A STOCK WIN16 PROGRAM LEAVES XP's SHARED WOW VDM RESIDENT: until that ntvdm.exe ends,
rem   any further Win16 program joins it (stock), IFEO or not. End it before comparing a
rem   Win16 program under NTVDMEX.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set DEMO=%SH%\demo
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set S=%SH%\debug\out\handstock.txt
del /q "%SH%\debug\out\handstock_done.txt" >nul 2>&1
echo ---- IFEO before ---- > "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- IFEO removed; starting STOCK ---- >> "%S%"
if not "%~2"=="" start "" /D "%DEMO%\%~1" "%DEMO%\%~1\%~2"
if not "%~4"=="" start "" /D "%DEMO%\%~3" "%DEMO%\%~3\%~4"
ping -n 11 127.0.0.1 >nul
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
echo ---- IFEO after ---- >> "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (echo ---- restored target EXISTS ---- >> "%S%") else (echo ---- ****** RESTORED TARGET IS MISSING -- THE RIG IS BROKEN ****** ---- >> "%S%")
echo ---- running ---- >> "%S%"
tasklist /fi "imagename eq ntvdm.exe" >> "%S%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%S%" 2>&1
echo done > "%SH%\debug\out\handstock_done.txt"
