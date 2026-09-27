@echo off
rem clip16.bat -- #160 the Win16 clipboard, both directions, both paths, through the menus.
rem   Notepad: its text box is a real Win32 EDIT, so Edit > Paste / Copy go through
rem            WM_PASTE / WM_COPY to that control (and Paste must not be greyed).
rem   Calc:    calls GetClipboardData / SetClipboardData itself -- the krnl386 bridge.
rem Report: debug\out\clip16.txt (+ clip16_*.bmp, clip16_*_host.log). Closes what it opens.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set REP=%OUT%\clip16.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == clip16 %DATE% %TIME%
del /q "%OUT%\clip16_*" "%L%" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul

rem ---- Notepad: paste host text, type zzz, select all, copy, read it back
"%R%" clipset "HOST TEXT 160"
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
cd /d "%W16%\notepad"
start "" "%W16%\notepad\NOTEPAD.EXE"
ping -n 16 127.0.0.1 >nul
"%R%" fg "Notepad - (Untitled)"
ping -n 2 127.0.0.1 >nul
rem Alt, E, P = Edit > Paste
"%R%" key 18
ping -n 2 127.0.0.1 >nul
"%R%" key 69
ping -n 2 127.0.0.1 >nul
"%R%" key 80
ping -n 3 127.0.0.1 >nul
"%R%" key 90 3
ping -n 2 127.0.0.1 >nul
"%R%" shot "%OUT%\clip16_notepad_paste.bmp"
"%R%" clipset "NOT COPIED"
rem Alt, E, A = Select All; Alt, E, C = Copy
"%R%" key 18
ping -n 2 127.0.0.1 >nul
"%R%" key 69
ping -n 2 127.0.0.1 >nul
"%R%" key 65
ping -n 2 127.0.0.1 >nul
"%R%" key 18
ping -n 2 127.0.0.1 >nul
"%R%" key 69
ping -n 2 127.0.0.1 >nul
"%R%" key 67
ping -n 3 127.0.0.1 >nul
"%R%" clipget
"%R%" shot "%OUT%\clip16_notepad_copy.bmp"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\clip16_notepad_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

rem ---- Calc: paste "42" (Calc types it), copy the display back out
"%R%" clipset "42"
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
cd /d "%W16%\calc"
start "" "%W16%\calc\CALC.EXE"
ping -n 16 127.0.0.1 >nul
"%R%" fg "Calculator"
ping -n 2 127.0.0.1 >nul
"%R%" key 18
ping -n 2 127.0.0.1 >nul
"%R%" key 69
ping -n 2 127.0.0.1 >nul
"%R%" key 80
ping -n 4 127.0.0.1 >nul
"%R%" shot "%OUT%\clip16_calc_paste.bmp"
"%R%" clipset "NOT COPIED"
"%R%" key 18
ping -n 2 127.0.0.1 >nul
"%R%" key 69
ping -n 2 127.0.0.1 >nul
"%R%" key 67
ping -n 3 127.0.0.1 >nul
"%R%" clipget
copy /y "%OUT%\ntvdmhost.log" "%OUT%\clip16_calc_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

type "%L%" >> "%REP%" 2>&1
>> "%REP%" echo -- host log lines, Notepad:
findstr /c:"Clipboard" /c:"CLIP " /c:"edit command" /c:"EM_ n=" "%OUT%\clip16_notepad_host.log" >> "%REP%" 2>&1
>> "%REP%" echo -- host log lines, Calc:
findstr /c:"Clipboard" /c:"CLIP " /c:"edit command" "%OUT%\clip16_calc_host.log" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
