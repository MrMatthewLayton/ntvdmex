@echo off
rem ============================================================================
rem  dosstock.bat -- run a DOS probe under STOCK ntvdm, to turn our numbers into
rem                  verdicts.  (s79)
rem
rem     dosstock.bat <probe.com>        e.g. dosstock.bat P_INT53.COM
rem
rem  INT 21h AH=53h's AL sub-functions are a PRIVATE NT extension: XP's own
rem  COMMAND.COM reads the answer out of AL, and NTDOS.SYS is the only thing that
rem  implements them. Real MS-DOS 6.22 cannot be asked -- the documented AH=53h
rem  BUILDS a DPB from a caller-supplied BPB and a fabricated pointer HANGS it
rem  (measured twice). So stock ntvdm on this box is the only oracle there is.
rem
rem  âââ THE IFEO KEY IS THE HAZARD AND IT IS THE WHOLE RISK OF THIS SCRIPT.
rem    * Left ABSENT, every later test on this box silently measures stock ntvdm
rem      and the logs look entirely plausible while grading the wrong emulator.
rem    * Restored to the WRONG PATH, every DOS and Win16 launch fails -- that is
rem      how this rig has been bricked before.
rem  â  stockdump.bat CANNOT be reused: it restores a hardcoded C:\ntvdmex path,
rem    which is NOT where this rig's host lives. This restores what rt.bat and
rem    w16launch.bat both assert, and proves it afterwards.
rem  â  The probe prints to STDOUT (#PROBE / CASE= / #END), so the run is redirected
rem    rather than collected from a host log -- stock writes no host log.
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set OUT=%SH%\debug\out
set D=%SH%\debug\tests\dos
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set P=%1
set A=%2 %3 %4
set S=%OUT%\dosstock_state.txt
set R=%OUT%\dosstock_out.txt

del /q "%R%" >nul 2>&1
del /q "%OUT%\dosstock_done.txt" >nul 2>&1

if not exist "%D%\%P%" (
  echo NO SUCH PROBE: %D%\%P% > "%S%"
  echo done > "%OUT%\dosstock_done.txt"
  goto :eof
)

taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1

echo ---- IFEO before ---- > "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- IFEO removed, running STOCK ---- >> "%S%"

cd /d "%D%"
rem ââ START + TIMED KILL, NEVER A BLOCKING CALL. The first cut ran the
rem   guest inline and waited for it. That is fine for a PROBE, which exits -- and it
rem   left the IFEO key REMOVED the moment the guest was COMMAND.COM /p, which sits at
rem   its prompt for ever. The restore below is after the run, so it never happened:
rem   the box was left routing every DOS and Win16 launch to stock, silently.
rem   w16stack.bat had this right and I did not copy it. A bracket whose restore can be
rem   skipped by the thing it brackets is not a bracket.
start "" "%D%\%P%" %A% > "%R%" 2>&1
ping -n 16 127.0.0.1 >nul
echo ---- run window over ---- >> "%S%"

taskkill /f /im ntvdm.exe >nul 2>&1

rem -- RESTORE, UNCONDITIONALLY, and prove it.
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
echo ---- IFEO after ---- >> "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (
  echo ---- restored target EXISTS ---- >> "%S%"
) else (
  echo ---- ****** RESTORED TARGET IS MISSING -- THE RIG IS BROKEN ****** ---- >> "%S%"
)
echo done > "%OUT%\dosstock_done.txt"
