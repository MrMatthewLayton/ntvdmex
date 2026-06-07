@echo off
rem ===========================================================================
rem  runtest.bat -- one-click M3 gate. Run from the XP DESKTOP (interactive
rem  session -- the only one where launching a 16-bit program fires the IFEO
rem  ntvdm.exe -> ntvdmhost.exe redirect). It pulls everything it needs from the
rem  host over TFTP, runs the I/O-trap test, prints the log, then offers the
rem  DirectDraw demo. Nothing to remember; just run it.
rem ===========================================================================
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

echo === Pulling fresh files from the host (10.0.2.2) ===
del /f /q %N%\ntvdmhost.exe %N%\ioprobe.com %N%\dosstub.com %N%\present_demo.exe %N%\ntvdmhost.log >nul 2>&1
tftp -i 10.0.2.2 GET ntvdmhost.exe    %N%\ntvdmhost.exe
tftp -i 10.0.2.2 GET ioprobe.com      %N%\ioprobe.com
tftp -i 10.0.2.2 GET dosstub.com      %N%\dosstub.com
tftp -i 10.0.2.2 GET present_demo.exe %N%\present_demo.exe

echo === Pointing IFEO ntvdm.exe -^> ntvdmhost.exe ===
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f >nul
echo %N%\ioprobe.com> %N%\target.txt

echo.
echo ========================= TEST 1: I/O trap =========================
echo Running ioprobe.com in V86 (a brief VDM flash is normal) ...
start /wait %N%\dosstub.com
echo ------------------------- ntvdmhost.log -------------------------
type %N%\ntvdmhost.log
echo -----------------------------------------------------------------
echo.
echo PASS looks like: three "IO out" lines, one "IO in ... -^> 0x34",
echo then "STAGE2: complete".  If you see "STAGE2: stop event=0x..",
echo copy the whole block (incl. info / bytes / VTIB lines) back to me.
echo.
echo ===================== TEST 2: DirectDraw demo =====================
echo Press a key to launch present_demo.exe
echo   (animated colour pattern; Alt+Enter = fullscreen, Esc = quit)
pause >nul
%N%\present_demo.exe

echo.
echo Done. Re-run this file any time to repeat both tests.
pause
endlocal
