@echo off
rem menushot.bat -- photograph the MENU BAR after the s60 restructure.
rem
rem WHAT IS BEING CHECKED: the bar should read  File  Edit  View  Tools  Help  --
rem five tops, not the previous seven (Machine, Capture and Debug are now submenus
rem of Tools, and Settings / Install / Uninstall / Installation Status moved there
rem out of File).
rem
rem ⚠ THE POPUPS CANNOT BE OPENED FROM HERE, AND THAT IS NOT A GAP IN THIS SCRIPT.
rem   The host swallows WM_SYSKEYDOWN on purpose ("never let DefWindowProc open the
rem   menu bar"), so Alt does not drop the menu and rigshot's `key` verb sends single
rem   keys with no modifiers. Opening Tools needs a click at a coordinate that
rem   depends on where the window landed. So this proves the BAR; the popup contents
rem   and the Edit greying in a graphics mode still want a human look.
rem
rem ⚠ LEAVES THE HOST RUNNING for that look. Which means the next runner will
rem   taskkill it -- so clear the consecutive-failed-start counter, or three of these
rem   in a row and the host uninstalls its own IFEO key (s59).
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set R=%BM%\rigshot.exe
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
del /q C:\ntvdmex\startfail.txt >nul 2>&1
del /q "%RES%\menushot.txt" >nul 2>&1
del /q "%RES%\menushot_done.txt" >nul 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1

rem A TEXT-MODE guest, so the Edit items should be ENABLED in this shot -- the
rem graphics-mode half of that claim needs a different guest and a click.
if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\COMMAND.COM" C:\test\ >nul
echo C:\test\COMMAND.COM> C:\ntvdmex\target.txt
del /q C:\ntvdmex\autoexit >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

cd /d C:\test
start "" C:\test\dosstub.com
ping -n 9 127.0.0.1 >nul

echo ---- is it up, and is it OURS? ---- > "%RES%\menushot.txt"
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >> "%RES%\menushot.txt"
if not exist C:\ntvdmex\ntvdmhost.log echo ^*^*^* NO HOST LOG -- stock ntvdm ran this >> "%RES%\menushot.txt"
"%R%" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" shot "%RES%\menushot.bmp" >nul 2>&1
echo ---- windows on the desktop ---- >> "%RES%\menushot.txt"
"%R%" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%RES%\menushot.txt" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\menushot_host.txt" >nul 2>&1
echo done> "%RES%\menushot_done.txt"
endlocal
