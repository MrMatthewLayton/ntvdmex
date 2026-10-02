@echo off
rem mgrtest.bat -- GH #281: the NTVDMEX manager's lifetime, unattended (s88).
rem   1. no manager running   2. Notepad starts -> manager appears
rem   3. Clock starts too      4. both closed -> manager gone a few seconds later
rem Writes debug\out\mgrtest.txt. Uses the same IFEO + launch shape as w16close.bat.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\mgrtest.txt
del /q "%OUT%\mgrtest_done.txt" >nul 2>&1
> "%R%" echo == mgrtest %DATE% %TIME%
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
taskkill /f /im ntvdmex.exe >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
ping -n 3 127.0.0.1 >nul
>> "%R%" echo -- 1. before anything:
tasklist /fi "imagename eq ntvdmex.exe" >> "%R%" 2>&1
cd /d "%W16%\notepad"
start "" "%W16%\notepad\NOTEPAD.EXE"
ping -n 16 127.0.0.1 >nul
>> "%R%" echo -- 2. Notepad running:
tasklist /fi "imagename eq ntvdmex.exe" >> "%R%" 2>&1
cd /d "%W16%\clock"
start "" "%W16%\clock\CLOCK.EXE"
ping -n 16 127.0.0.1 >nul
>> "%R%" echo -- 3. Notepad + Clock running:
tasklist /fi "imagename eq ntvdmex.exe" >> "%R%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
"%RIG%\rigshot.exe" shot "%OUT%\mgrtest.bmp" >nul 2>&1
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
"%RIG%\rigshot.exe" close "Clock"
ping -n 3 127.0.0.1 >nul
"%RIG%\rigshot.exe" close "Notepad - (Untitled)"
ping -n 16 127.0.0.1 >nul
>> "%R%" echo -- 4. both closed, 15 s later:
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
tasklist /fi "imagename eq ntvdmex.exe" >> "%R%" 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
>> "%R%" echo == done %TIME%
> "%OUT%\mgrtest_done.txt" echo done
