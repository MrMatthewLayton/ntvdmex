@echo off
rem stockdump.bat -- ask STOCK ntvdm what a correct Win16 loader puts in memory.
rem GH #128, session 33.
rem
rem THE POINT. Our loader gives krnl386's segments 2-4 a size-0 GlobalAlloc
rem placeholder that nothing ever fills, so LoadSegment(2) fails. Stock ntvdm on
rem THIS box implements that contract, against THIS krnl386.exe. So it is the
rem oracle, exactly as rt_stock.bat is the oracle for the keyboard.
rem
rem It drops the IFEO Debugger value (so SYSEDIT routes to stock, not to us),
rem launches SYSEDIT, dumps the live VDM from outside with vdmdump.exe, and puts
rem the key back. THE RESTORE IS ON EVERY EXIT PATH ON PURPOSE: leaving that key
rem absent silently turns every later test into a stock run, and the logs would
rem look entirely plausible while measuring the wrong emulator.
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set OUT=C:\ntvdmex\stockdump

rem -- clean slate. A stale artefact is worse than a missing one: a "before" and
rem    an "after" once analysed the same hours-old file.
del /q %OUT%.txt %OUT%.bin %OUT%.blk %OUT%.con >nul 2>&1
del /q "%RES%\stockdump.txt" "%RES%\stockdump.bin" "%RES%\stockdump.blk" >nul 2>&1
del /q "%RES%\stockdump_con.txt" "%RES%\stockdump_state.txt" >nul 2>&1
del /q "%RES%\stockdump_done.txt" >nul 2>&1

if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\vdmdump.exe" C:\ntvdmex\ >nul
copy /y "%BM%\needles.txt" C:\ntvdmex\ >nul

taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
taskkill /f /im sysedit.exe >nul 2>&1

rem -- record the key BEFORE touching it, then drop it.
echo ---- IFEO before ---- > "%RES%\stockdump_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\stockdump_state.txt" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- IFEO removed, running stock ---- >> "%RES%\stockdump_state.txt"

rem -- a Win16 app: XP starts a WOW VDM (ntvdm.exe) and loads krnl386 into it.
start "" C:\WINDOWS\SYSTEM32\sysedit.exe
ping -n 9 127.0.0.1 >nul
echo ---- tasklist at dump time ---- >> "%RES%\stockdump_state.txt"
tasklist /fi "imagename eq ntvdm.exe" >> "%RES%\stockdump_state.txt" 2>&1

C:\ntvdmex\vdmdump.exe %OUT% --proc ntvdm.exe --needles C:\ntvdmex\needles.txt > %OUT%.con 2>&1

taskkill /f /im ntvdm.exe >nul 2>&1

rem -- restore, unconditionally, and prove it.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo ---- IFEO after ---- >> "%RES%\stockdump_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\stockdump_state.txt" 2>&1

copy /y %OUT%.txt "%RES%\stockdump.txt" >nul 2>&1
copy /y %OUT%.bin "%RES%\stockdump.bin" >nul 2>&1
copy /y %OUT%.blk "%RES%\stockdump.blk" >nul 2>&1
copy /y %OUT%.con "%RES%\stockdump_con.txt" >nul 2>&1
echo done > "%RES%\stockdump_done.txt"
