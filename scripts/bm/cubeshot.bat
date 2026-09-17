@echo off
rem cubeshot.bat -- drive VESACUBE's MENU the way a person would and photograph it. (s74b)
rem   Launches the demo interactively through the IFEO route (no autoexit, no target.txt),
rem   presses the menu letters with rigshot key, captures the desktop with rigshot shot.
rem   Report: debug\out\cubeshot_<letter>.bmp per mode, cubeshot_done.txt when finished.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LETTERS=%*
if "%LETTERS%"=="" set LETTERS=65 66 67
del /q "%OUT%\cubeshot_done.txt" >nul 2>&1
del /q "%OUT%\cubeshot_*.bmp" "%OUT%\shot_manual_*.bmp" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
del /q "%OUT%\startfail.txt" "%CFG%\autoexit" "%CFG%\target.txt" >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
cd /d "%SH%\demo\msdos\vesacube"
start "" "%SH%\demo\msdos\vesacube\VESACUBE.COM"
ping -n 7 127.0.0.1 >nul
"%RIG%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine"
"%RIG%\rigshot.exe" click 400 560
ping -n 2 127.0.0.1 >nul
ping -n 2 127.0.0.1 >nul
"%RIG%\rigshot.exe" shot "%OUT%\cubeshot_menu.bmp"
for %%L in (%LETTERS%) do (
  "%RIG%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine"
"%RIG%\rigshot.exe" click 400 560
ping -n 2 127.0.0.1 >nul
  "%RIG%\rigshot.exe" key %%L
  ping -n 6 127.0.0.1 >nul
  rem the host's own Capture > Take Screenshot (IDM_CAP_SHOT = 8): a desktop BitBlt cannot
  rem see a DirectDraw fullscreen surface, the host's frame copy can -- shot_manual_NN.bmp
  "%RIG%\rigshot.exe" cmd 8
  ping -n 2 127.0.0.1 >nul
  "%RIG%\rigshot.exe" key 27
  ping -n 3 127.0.0.1 >nul
)
"%RIG%\rigshot.exe" key 27
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
echo done > "%OUT%\cubeshot_done.txt"
