@echo off
rem zarcmp.bat -- ZAR under NTVDMEX and under STOCK ntvdm, captured both ways.  #23
rem
rem WHY THE STOCK HALF IS THE WHOLE POINT. ZAR is the third of the three original
rem games and the only one never made to run. The first run under NTVDMEX (s58)
rem reached protected mode and then fell into a repeating #GP reflect loop inside
rem DOS4GW's own exception handler, BEFORE any video mode is set -- so whatever is
rem wrong, it is not the VESA gap the score model has always attributed it to.
rem
rem That leaves one question worth answering before spending a day on it: does the
rem STOCK VDM run this program at all? If it does, the difference is ours and there
rem is a target. If it does not, ZAR needs something neither host provides, and the
rem honest thing is to say so rather than grind.
rem
rem ⚠ #26 FLAGS A DISPLAY-WEDGE RISK for stock ntvdm on graphics-mode DOS programs,
rem   which is exactly what this is. So the stock half is KILLED after its capture
rem   rather than left on the desktop, and the IFEO restore happens BEFORE the kill
rem   so a wedge cannot strand the box in "everything measures stock" state.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\zarcmp.txt

del /q "%RES%\zarcmp_done.txt" >nul 2>&1
del /q "%RES%\zar_ours.bmp" "%RES%\zar_stock.bmp" >nul 2>&1
> "%LOG%" echo ==== zarcmp: %DATE% %TIME% ====

rem -- stage the game once; both halves run the SAME copy from the SAME path.
if not exist C:\game\ZAR.EXE (
  echo staging C:\game from the share... >> "%LOG%"
  rmdir /s /q C:\game >nul 2>&1
  xcopy /e /i /y "%RES%\zar" C:\game >nul 2>&1
)
rem ⚠ LIST THE WHOLE DIRECTORY, not just the .EXE. ZAR reads USER1.CFG, ZAR.CFG and
rem   four multi-megabyte .SFS archives; a game that exits in half a second having
rem   printed nothing looks identical whether the fault is ours or the staging's.
dir C:\game >> "%LOG%" 2>&1

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

rem ---- 1. OURS ---------------------------------------------------------------
echo. >> "%LOG%"
echo ==== 1. UNDER NTVDMEX ==== >> "%LOG%"
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo C:\game\ZAR.EXE> C:\ntvdmex\target.txt
cd /d C:\game
start "" C:\game\ZAR.EXE
ping -n 31 127.0.0.1 >nul
del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\zar_ours.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdmhost.exe" >> "%LOG%" 2>&1
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\zar_ours.log" >nul 2>&1

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
ping -n 3 127.0.0.1 >nul

rem ---- 2. STOCK --------------------------------------------------------------
echo. >> "%LOG%"
echo ==== 2. UNDER STOCK NTVDM ==== >> "%LOG%"
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [zarcmp] IFEO Debugger REMOVED >> "%LOG%"
cd /d C:\game
start "" C:\game\ZAR.EXE
ping -n 31 127.0.0.1 >nul

rem ── RESTORE FIRST, CAPTURE SECOND. A wedged stock VDM must not be able to strand
rem    this box with the Debugger value absent -- everything later would measure
rem    stock while producing entirely plausible logs.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
reg query "%IFEO%" /v Debugger | find "ntvdmhost" >nul
if errorlevel 1 (echo [zarcmp] !! IFEO RESTORE FAILED >> "%LOG%") else (echo [zarcmp] IFEO restored >> "%LOG%")

del /q "%RES%\rigshot.txt" >nul 2>&1
"%BM%\rigshot.exe" shot "%RES%\zar_stock.bmp" >nul 2>&1
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%LOG%" 2>&1
tasklist /fi "imagename eq ntvdm.exe" >> "%LOG%" 2>&1

taskkill /f /im ntvdm.exe >nul 2>&1
echo DONE > "%RES%\zarcmp_done.txt"
