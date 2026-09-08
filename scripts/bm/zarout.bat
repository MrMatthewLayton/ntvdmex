@echo off
rem zarout.bat -- ZAR's OWN STDOUT, under both hosts, into two files.  #23
rem
rem WHY. Session 58 concluded "ZAR runs under stock" from a screenshot that was
rem ENTIRELY BLACK -- stock ntvdm takes a graphics-mode DOS program FULLSCREEN, and
rem a desktop BitBlt of that comes back black whether the guest is running happily
rem or sitting on an error. That is not evidence, it is the absence of evidence, and
rem the whole ZAR investigation is resting on it.
rem
rem DOS/4GW writes its fault report to STDOUT ("Error [35]: General Protection
rem Fault in DOS4GW.EXE at ..."), so redirection answers the question directly and
rem in text, with no screenshot to interpret:
rem   - stock file EMPTY / no error  -> stock really does get further; the gap is ours
rem   - stock file has the SAME error -> ZAR fails under stock too, and #23 is not a
rem     defect of ours at all. That would change the whole line of attack, which is
rem     why it is worth one run to find out.
rem
rem ⚠ A FRESH OUTPUT NAME PER HALF, and both deleted first. A run that wedges leaves
rem   the file locked, `del` then fails silently and the next half is read as the
rem   first one's content -- the stale-artefact trap, which has cost this project a
rem   whole comparison before.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set LOG=%RES%\zarout.txt

del /q "%RES%\zarout_done.txt" >nul 2>&1
del /q "%RES%\zarout_ours.txt" "%RES%\zarout_stock.txt" >nul 2>&1
del /q C:\game\o_ours.txt C:\game\o_stock.txt >nul 2>&1
> "%LOG%" echo ==== zarout: ZAR stdout under both hosts, %DATE% %TIME% ====

"%BM%\rigshot.exe" close "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 3 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >> "%LOG%" 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1

cd /d C:\game

rem ---- 1. OURS ---------------------------------------------------------------
echo. >> "%LOG%"
echo ==== 1. NTVDMEX ==== >> "%LOG%"
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul 2>&1
echo C:\game\ZAR.EXE> C:\ntvdmex\target.txt
rem ⚠ NEVER `start /wait` OUR HOST. Without the autoexit marker the host KEEPS ITS
rem   WINDOW OPEN after the guest ends, so /wait never returns and the run wedges the
rem   box (measured, s58 -- it took a controld kill to clear). The marker is what
rem   rt.bat uses to make the host self-exit with its guest; belt and braces, this
rem   also does not wait and kills on a timer.
echo.> C:\ntvdmex\autoexit
start "" cmd /c "C:\game\ZAR.EXE > C:\game\o_ours.txt 2>&1"
ping -n 26 127.0.0.1 >nul
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q C:\ntvdmex\autoexit >nul 2>&1
ping -n 3 127.0.0.1 >nul
copy /y C:\game\o_ours.txt "%RES%\zarout_ours.txt" >nul 2>&1
echo --- ours --- >> "%LOG%"
type C:\game\o_ours.txt >> "%LOG%" 2>&1

rem ---- 2. STOCK --------------------------------------------------------------
echo. >> "%LOG%"
echo ==== 2. STOCK NTVDM ==== >> "%LOG%"
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo [zarout] IFEO Debugger REMOVED >> "%LOG%"
rem Same rule for stock: no /wait. #26 flags a display-wedge risk for graphics-mode
rem DOS under stock, and a wedged VDM behind /wait is a box only a hand can clear.
start "" cmd /c "C:\game\ZAR.EXE > C:\game\o_stock.txt 2>&1"
ping -n 26 127.0.0.1 >nul

rem ── RESTORE FIRST. A wedged stock VDM must not be able to strand this box with
rem    the Debugger value absent -- everything later would measure stock.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
reg query "%IFEO%" /v Debugger | find "ntvdmhost" >nul
if errorlevel 1 (echo [zarout] !! IFEO RESTORE FAILED >> "%LOG%") else (echo [zarout] IFEO restored >> "%LOG%")
taskkill /f /im ntvdm.exe >nul 2>&1
copy /y C:\game\o_stock.txt "%RES%\zarout_stock.txt" >nul 2>&1
echo --- stock --- >> "%LOG%"
type C:\game\o_stock.txt >> "%LOG%" 2>&1

echo. >> "%LOG%"
dir C:\game\o_ours.txt C:\game\o_stock.txt >> "%LOG%" 2>&1
echo DONE > "%RES%\zarout_done.txt"
