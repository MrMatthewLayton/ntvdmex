@echo off
rem wowcompare.bat -- run one Win16 program under NTVDMEX **and** under STOCK ntvdm
rem                   at the same time, on the same desktop.  GH #128, session 44.
rem
rem   wowcompare.bat                       -- C:\WIN16\NOTEPAD.EXE
rem   wowcompare.bat C:\WIN16\TERMINAL.EXE -- something else
rem
rem WHY BOTH AT ONCE. Every claim this project makes about a Win16 guest is a claim
rem about a difference: "its own menu", "its own icon", "its own caption". A
rem screenshot of ours alone cannot settle one of those -- the reader has to know
rem what the same program looks like when Windows runs it. Stock ntvdm on THIS box
rem is that oracle, and the only way to compare two windows fairly is to have them
rem on the screen together.
rem
rem ⚠⚠ THE IFEO KEY IS THE HAZARD IN THIS FILE. Interception IS the `Debugger` value
rem   on ntvdm.exe: present, a Win16 launch comes to us; absent, it goes to stock.
rem   So this file deliberately removes it for ONE `start` and puts it back
rem   immediately -- and the restore is unconditional and VERIFIED, because a run
rem   that left it absent would make every later test silently measure stock ntvdm
rem   while producing entirely plausible logs.
rem   ⇒ ORDER MATTERS: ours goes first, WITH the key, and keeps running; stock goes
rem     second, without it. A process already started is unaffected by the key.
rem
rem ⚠ IT DOES NOT KILL ANYTHING AT THE END. Both guests are left on the desktop to
rem   be compared by hand. Stop ours from the tray icon or its own window; stop
rem   stock's by closing its window.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set LOG=%RES%\wowcompare.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set TARGET=%~1
if "%TARGET%"=="" set TARGET=C:\WIN16\NOTEPAD.EXE

del /q "%LOG%" >nul 2>&1
del /q "%RES%\wowcompare.bmp" >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex

rem ── !! KILL FIRST, THEN WAIT, THEN COPY, THEN PROVE IT. A host left running from
rem    a previous invocation holds C:\ntvdmex\ntvdmhost.exe open, `copy ... >nul`
rem    then fails SILENTLY, and the OLD binary runs while this script reports
rem    success. That has happened. The wait, the errorlevel and the two listings
rem    exist so it cannot happen quietly again.
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ > "%LOG%" 2>&1
if errorlevel 1 echo [compare] !! COPY FAILED -- THE OLD HOST IS ABOUT TO RUN >> "%LOG%"
echo ==== deployed vs source (sizes must match) ==== >> "%LOG%"
dir "%BM%\ntvdmhost.exe" C:\ntvdmex\ntvdmhost.exe >> "%LOG%" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

rem -- 0 = a blocked GetMessage waits forever, which is what a real Win16 task does
rem    and what a session someone is going to click around in needs.
rem  !! `echo 0>file` is a redirection of handle 0, not "write 0". Parenthesise it.
(echo 0)> "%RES%\wowidle.txt"
echo %TARGET%> C:\ntvdmex\target.txt

echo. >> "%LOG%"
echo ==== 1. UNDER NTVDMEX (IFEO Debugger present) ==== >> "%LOG%"
reg query "%IFEO%" /v Debugger >> "%LOG%" 2>&1
if errorlevel 1 echo [compare] !! NO IFEO KEY -- THIS WOULD HAVE MEASURED STOCK >> "%LOG%"
start "" "%TARGET%"
ping -n 16 127.0.0.1 >nul
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo ==== 2. UNDER STOCK NTVDM (IFEO Debugger removed for one start) ==== >> "%LOG%"
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [compare] IFEO Debugger REMOVED >> "%LOG%"
start "" "%TARGET%"
ping -n 12 127.0.0.1 >nul

rem ── THE RESTORE. Unconditional, immediately, and then READ BACK -- see the note
rem    at the top. Anything after this point may fail; the key is already back.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [compare] IFEO Debugger RESTORED -- read back below >> "%LOG%"
reg query "%IFEO%" /v Debugger >> "%LOG%" 2>&1

echo. >> "%LOG%"
echo ==== both should now be running ==== >> "%LOG%"
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdm.exe"     >> "%LOG%" 2>&1
"%BM%\rigshot.exe" list >> "%LOG%" 2>&1
"%BM%\rigshot.exe" shot "%RES%\wowcompare.bmp" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\wowcompare_host.txt" >nul 2>&1
echo [compare] BOTH LEFT RUNNING -- stop ours from the tray icon, stock's by >> "%LOG%"
echo [compare] closing its window.                                           >> "%LOG%"
echo done> "%RES%\wowcompare_done.txt"
