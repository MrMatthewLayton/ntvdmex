@echo off
rem rt_stock.bat -- run a target under STOCK ntvdm, not NTVDMEX.  (GH #26)
rem
rem THE POINT. Everything we knew about "what real DOS does" for the keyboard came
rem from memory, and memory was wrong twice. Stock ntvdm on THIS box, with THIS
rem keyboard and THIS XP typematic setting, is the executable oracle -- and it is
rem the one comparison host #26 never wired up.
rem
rem It drops the IFEO Debugger value that points ntvdm.exe at our host, runs the
rem target, then puts it back. The restore is duplicated on every exit path on
rem purpose: leaving that key absent silently turns every later test into a stock
rem run, and the logs would look entirely plausible while measuring the wrong
rem emulator.
rem
rem OUTPUT IS REDIRECTED TO A FILE, because there is no host log under stock ntvdm
rem and the window closes the instant the program exits -- which is exactly how the
rem first attempt at this lost its results. The prompt is echoed here, by the batch
rem file, so it survives the redirection.
rem
rem TEXT-MODE TARGETS ONLY unless you have decided otherwise: #26 flagged a
rem display-wedge risk for stock ntvdm on graphics-mode DOS programs.
set T=%1
set BM=C:\Documents and Settings\All Users\Documents\ntvdmex\bm
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

taskkill /f /im ntvdmhost.exe >nul 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [stock] IFEO Debugger removed > "%RES%\stock_state.txt"

if not exist C:\test md C:\test
copy /y "%BM%\tests\%T%" C:\test\ >nul
del /q C:\test\stock_out.txt >nul 2>&1
cd /d C:\test

echo.
echo ============================================================
echo   RUNNING %T% UNDER STOCK NTVDM
echo   HOLD THE UP ARROW DOWN and keep holding for ~8 seconds.
echo   Output goes to a file, so the window closing is harmless.
echo ============================================================
echo.
start /wait "" cmd /c "C:\test\%T% > C:\test\stock_out.txt 2>&1"

copy /y C:\test\stock_out.txt "%RES%\result_stock_%T%.txt" >nul 2>&1

reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [stock] IFEO Debugger restored >> "%RES%\stock_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\stock_state.txt" 2>&1
echo.
echo Done. Result copied to result_stock_%T%.txt on the share.
