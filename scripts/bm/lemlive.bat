@echo off
rem lemlive.bat -- reproduce a BY-HAND Lemmings launch (no headless flags, no key
rem script) and drive it with rigshot the way a person would: keys to the briefing,
rem then real clicks through the real window proc. (s68: "never made it to the game
rem screen" by hand while the headless script did.) Leaves the host running.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set LOG=%OUT%\lemlive.txt
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%CFG%\autoexit" "%CFG%\keys.txt" "%CFG%\qimode.txt" "%CFG%\capture.flag" "%OUT%\startfail.txt" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" "%OUT%\lemlive*.bmp" "%SH%\rigshot.txt" >nul 2>&1
echo "%SH%\demo\msdos\Lemmings\VGALEMMI.EXE"> "%CFG%\target.txt"
echo ==== lemlive %DATE% %TIME% ==== > "%LOG%"
cd /d "%SH%\demo\msdos\Lemmings"
start "" "%RIG%\dosstub.com"
ping -n 9 127.0.0.1 >nul
"%R%" list >nul 2>&1
type "%SH%\rigshot.txt" >> "%LOG%"
"%R%" fg "%~1" >nul 2>&1
type "%SH%\rigshot.txt" >> "%LOG%"
ping -n 2 127.0.0.1 >nul
"%R%" key 49 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" key 13 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" key 13 >nul 2>&1
ping -n 3 127.0.0.1 >nul
"%R%" key 13 >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" key 112 >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" key 32 >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" shot "%OUT%\lemlive1.bmp" >nul 2>&1
"%R%" click %2 %3 >nul 2>&1
ping -n 2 127.0.0.1 >nul
"%R%" shot "%OUT%\lemlive2.bmp" >nul 2>&1
"%R%" click %2 %3 >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" shot "%OUT%\lemlive3.bmp" >nul 2>&1
ping -n 4 127.0.0.1 >nul
"%R%" shot "%OUT%\lemlive4.bmp" >nul 2>&1
type "%SH%\rigshot.txt" >> "%LOG%"
echo ==== host log tail ==== >> "%LOG%"
copy /y "%OUT%\ntvdmhost.log" "%OUT%\lemlive_host.txt" >nul 2>&1
echo done>> "%LOG%"
endlocal
