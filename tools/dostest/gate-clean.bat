@echo off
rem ===========================================================================
rem  Manual real-V86 gate for the CLEAN host (ntvdmhost.exe) -- M2.6 slice 4.
rem
rem  RUN THIS INSIDE THE XP VM, from the desktop (an INTERACTIVE session) -- the
rem  only session where launching a 16-bit program fires the IFEO ntvdm.exe
rem  redirect. Unlike gate.bat (which gates the tools/vdmhost spike), this points
rem  the IFEO Debugger at the clean src/ host and reads ITS log (ntvdmhost.log).
rem
rem  Usage (in the VM):   gate-clean.bat            (tests memtest.com)
rem                       gate-clean.bat foo.com    (another program staged in build/)
rem
rem  Verdict to look for near the end of ntvdmhost.log:
rem      ==> DOS OUTPUT: [MEMTEST PASS]       (or "Hello..." for a hello program)
rem      STAGE2: complete                     (and NO "STAGE2: stop event=..." line)
rem ===========================================================================
setlocal
set N=C:\ntvdmex
set PROG=%1
if "%PROG%"=="" set PROG=memtest.com
if not exist %N% md %N%

echo Pulling ntvdmhost.exe + %PROG% from the host (10.0.2.2) ...
del /f /q %N%\ntvdmhost.exe >nul 2>&1
tftp -i 10.0.2.2 GET ntvdmhost.exe %N%\ntvdmhost.exe
del /f /q %N%\%PROG% >nul 2>&1
tftp -i 10.0.2.2 GET %PROG% %N%\%PROG%
if not exist %N%\dosstub.com tftp -i 10.0.2.2 GET dosstub.com %N%\dosstub.com

echo Pointing IFEO Debugger(ntvdm.exe) at the CLEAN host (ntvdmhost.exe) ...
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f

echo %N%\%PROG%>%N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running %PROG% through the clean host (a brief VDM flash is normal) ...
start /wait %N%\dosstub.com

echo.
echo ===================== ntvdmhost.log =====================
type %N%\ntvdmhost.log
echo ========================================================
echo.
echo Copy the "DOS OUTPUT" / "STAGE2:" lines back to the host.
echo To switch back to the spike, run gate.bat (it re-points the IFEO at vdmhost.exe).
pause
endlocal
