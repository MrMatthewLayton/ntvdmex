@echo off
rem ===========================================================================
rem  Manual real-V86 integration gate for the DOS kernel.
rem
rem  RUN THIS INSIDE THE XP VM, from the desktop (an INTERACTIVE session) -- that
rem  is the only session where launching a 16-bit program fires the IFEO
rem  ntvdm.exe -> vdmhost redirect. (A telnet/service session will NOT trigger it.)
rem
rem  It pulls the latest vdmhost.exe + target program from the host over TFTP,
rem  points vdmhost at the target, runs it on the real CPU, and prints the log so
rem  you can read the verdict and paste it back. No telnet, no agent, no polling --
rem  you are in control and can see exactly what happens.
rem
rem  Usage (in the VM):   gate.bat             (tests memtest.com)
rem                       gate.bat foo.com     (tests another program staged in build/)
rem
rem  Verdict to look for, near the end of the log:
rem      ==> DOS OUTPUT: [MEMTEST PASS]
rem      ==> DOS terminate (AH=4Ch), exit code AL=0x00000000     (0 = all checks passed)
rem ===========================================================================
setlocal
set N=C:\ntvdmex
set PROG=%1
if "%PROG%"=="" set PROG=memtest.com
if not exist %N% md %N%

echo Pulling latest binaries from the host (10.0.2.2) ...
del /f /q %N%\vdmhost.exe >nul 2>&1
tftp -i 10.0.2.2 GET vdmhost.exe %N%\vdmhost.exe
del /f /q %N%\%PROG% >nul 2>&1
tftp -i 10.0.2.2 GET %PROG% %N%\%PROG%
if not exist %N%\dosstub.com tftp -i 10.0.2.2 GET dosstub.com %N%\dosstub.com

echo %N%\%PROG%>%N%\target.txt
if exist %N%\vdmhost.log del /f /q %N%\vdmhost.log

echo Running %PROG% through vdmhost (a brief VDM flash is normal) ...
start /wait %N%\dosstub.com

echo.
echo ===================== vdmhost.log =====================
type %N%\vdmhost.log
echo =======================================================
echo.
echo Copy the "DOS OUTPUT" line and the "exit code AL=" line back to the host.
pause
endlocal
