@echo off
rem cmdex.bat -- MS-DOS 6.22's COMMAND.COM, INTERACTIVELY, for a human at the box.
rem
rem The counterpart to cmdcom.bat (which is headless and drives itself from
rem keys.txt).  Differences that matter, and both have cost a session before:
rem   * NO `autoexit` MARKER.  With it present the host self-exits when the guest
rem     does -- and for a headless run it also arms the 45-second deadline, so an
rem     interactive session would die under you mid-typing.  Deleted, not skipped.
rem   * qimode MUST BE 0, or the synthetic key script fights you for the keyboard.
rem     Checked and forced here rather than assumed.
rem
rem Type at it like DOS: dir, ver, vol, cls, set, type, copy, attrib, exit.
setlocal
set R=C:\Documents and Settings\All Users\Documents\ntvdmex
set N=C:\ntvdmex
set D=C:\dostest

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
ping -n 3 127.0.0.1 >nul

if not exist %N% md %N%
if not exist %D% md %D%
copy /y "%R%\bm\ntvdmhost.exe" %N%\ >nul
copy /y "%R%\bm\dosstub.com" %D%\ >nul
copy /y "%R%\guest\COMMAND.COM" %D%\ >nul
copy /y "%R%\guest\ATTRIB.EXE"  %D%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul

>%N%\target.txt echo %D%\COMMAND.COM
del /q %N%\autoexit >nul 2>&1
del /q %N%\ntvdmhost.log >nul 2>&1
>"%R%\qimode.txt" echo 0

cd /d %D%
start "" %D%\dosstub.com
endlocal
