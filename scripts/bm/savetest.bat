@echo off
rem savetest.bat -- the WHOLE Save As test in one ordered run: clean, launch,
rem drive the menu, type the name, then LOOK. One batch because doing the clean
rem from a separate script raced the save and deleted the evidence.
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set D=C:\Documents and Settings\Matthew
del /q "%RES%\savetest.txt" >nul 2>&1
del /q "%RES%\savetest_done.txt" >nul 2>&1
del /q "%D%\TEST.BMP" >nul 2>&1
del /q "%D%\Desktop\TEST.BMP" >nul 2>&1
call "%RES%\wowlive.bat" C:\WIN16\PBRUSH.EXE >nul 2>&1
ping -n 6 127.0.0.1 >nul
call "%RES%\wowkeys.bat" "Paintbrush - (Untitled)" 0x12 0x46 0x41 >nul 2>&1
ping -n 4 127.0.0.1 >nul
call "%RES%\wowkeys.bat" "Save As" 0x54 0x45 0x53 0x54 0x0D >nul 2>&1
ping -n 8 127.0.0.1 >nul
echo ==== Desktop (where the user chose) ==== > "%RES%\savetest.txt"
dir "%D%\Desktop\*.bmp" >> "%RES%\savetest.txt" 2>&1
echo ==== profile dir (where it used to land) ==== >> "%RES%\savetest.txt"
dir "%D%\*.bmp" >> "%RES%\savetest.txt" 2>&1
echo ==== is Paintbrush still alive? ==== >> "%RES%\savetest.txt"
"%RES%\bm\rigshot.exe" list >> "%RES%\savetest.txt" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\savetest_host.txt" >nul 2>&1
echo done> "%RES%\savetest_done.txt"
