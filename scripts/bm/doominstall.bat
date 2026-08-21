@echo off
rem doominstall.bat -- ONE-TIME. Extract Doom shareware to C:\DOOMS.
rem
rem The DEICE installer can only be unpacked by RUNNING it, and it must run under
rem XP's STOCK ntvdm rather than ours -- so this drops the IFEO Debugger redirect
rem first and PUTS IT BACK at the end. Leaving that key absent is the failure mode
rem worth fearing here: every later test would silently measure stock ntvdm while
rem the logs looked entirely plausible.
rem
rem Follow SETUP's prompts. Choose NO SOUND for the first bring-up -- we want one
rem variable at a time, and the sound path can be turned on afterwards now that the
rem OPL and SB16 work.
setlocal
set SH=%~dp0
reg delete "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /f >nul 2>&1
if exist C:\DINST rd /s /q C:\DINST
md C:\DINST
xcopy /e /i /y "%SH%doominst" C:\DINST >nul
cd /d C:\DINST
echo.
echo ============================================================
echo   Extracting Doom to C:\DOOMS under STOCK ntvdm.
echo   Follow SETUP. Choose NO SOUND for this first run.
echo ============================================================
echo.
call INSTALL.BAT
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
echo [doom] IFEO Debugger restored > "%SH%stock_state.txt"
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger >> "%SH%stock_state.txt" 2>&1
echo.
echo Done. IFEO restored to NTVDMEX. Now run doomrun.bat.
endlocal
