@echo off
rem stockdoom.bat -- run Doom under STOCK ntvdm, as the existence proof.
rem
rem THE QUESTION: does XP's own ntvdm get Doom past I_StartupTimer() on THIS box?
rem Everything we are building assumes it can. That was never measured.
rem
rem Doom's startup text is redirected to a file, because there is no host log under
rem stock and the window closes when it dies. The discriminator is whether the file
rem contains D_CheckNetGame / S_Init / HU_Init / ST_Init -- the lines that come AFTER
rem I_StartupTimer(), which is exactly where NTVDMEX stops.
rem
rem THE IFEO KEY IS RESTORED ON EVERY EXIT PATH. Leaving it absent would silently
rem turn every later test into a stock run, with logs that look entirely plausible.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [stockdoom] IFEO Debugger removed > "%RES%\stockdoom_state.txt"
del /q C:\DOOMS\stockout.txt >nul 2>&1
cd /d C:\DOOMS
start "" /d C:\DOOMS cmd /c "DOOM.EXE > C:\DOOMS\stockout.txt 2>&1"
ping -n 45 127.0.0.1 >nul
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [stockdoom] IFEO Debugger restored >> "%RES%\stockdoom_state.txt"
reg query "%IFEO%" /v Debugger >> "%RES%\stockdoom_state.txt" 2>&1
copy /y C:\DOOMS\stockout.txt "%RES%\result_stockdoom.txt" >nul 2>&1
echo [stockdoom] done >> "%RES%\stockdoom_state.txt"
