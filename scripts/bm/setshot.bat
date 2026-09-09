@echo off
rem setshot.bat -- open Tools > Settings and photograph it.
rem
rem WHY IT MATTERS MORE THAN IT LOOKS: a malformed DIALOGEX template does not draw
rem badly, it FAILS TO CREATE -- DialogBoxParam returns -1 and the menu item silently
rem does nothing. Adding a trackbar to the CPU page is exactly the kind of edit that
rem does that (an unknown window class, a style constant the resource compiler does
rem not know). So "the build succeeded" says nothing here; the dialog has to be seen.
rem
rem HOW IT GETS THERE WITHOUT KNOWING ANY COORDINATES BUT ONE:
rem   click the "Tools" menu title, then END, then ENTER.
rem   END selects the LAST item in an open menu, and Settings is deliberately last on
rem   the Tools menu (Tools > Options at the bottom is the convention). So the only
rem   fragile number is the menu title's position, and if that click misses, the shot
rem   shows a window with no menu open rather than the wrong dialog.
rem ⚠ Alt cannot be used to open the menu: the host swallows WM_SYSKEYDOWN on purpose
rem   so the guest gets the key, which is why this clicks instead.
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
del /q "%RES%\setshot_done.txt" >nul 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1

if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\COMMAND.COM" C:\test\ >nul
echo C:\test\COMMAND.COM> C:\ntvdmex\target.txt
del /q C:\ntvdmex\autoexit >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

cd /d C:\test
start "" C:\test\dosstub.com
ping -n 9 127.0.0.1 >nul

"%R%" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
rem the "Tools" title on the menu bar (window lands at 132,174 on this desktop)
"%R%" click 235 213 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" shot "%RES%\setshot_menu.bmp" >nul 2>&1
rem ⚠ END + ENTER DID NOT WORK -- the menu opened and nothing was chosen, so END is
rem   not selecting the last item here. Click the item instead: its position is read
rem   off setshot_menu.bmp, which this script takes one line earlier, so the number
rem   below is measured rather than guessed.
"%R%" click 270 354 >nul 2>&1
ping -n 5 127.0.0.1 >nul
"%R%" shot "%RES%\setshot.bmp" >nul 2>&1
"%R%" list >nul 2>&1
copy /y "%RES%\rigshot.txt" "%RES%\setshot.txt" >nul 2>&1
echo done> "%RES%\setshot_done.txt"
endlocal
