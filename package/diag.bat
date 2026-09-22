@echo off
rem diag.bat -- one pass that explains "nothing happens and there is no log".
rem
rem Writes debug\out\diag.txt. Send THAT file back. It uses nothing that is
rem missing from Windows 2000: no reg.exe, no tasklist.exe (neither ships with
rem 2000), no findstr switches beyond /C.
rem
rem The question it answers is WHICH of three different failures you have:
rem   A. Windows never launched NTVDMEX -- your DOS programs still run under
rem      Windows' own ntvdm.exe, which is why nothing is logged.
rem   B. NTVDMEX was launched and died before it could write its first line.
rem   C. NTVDMEX ran but could not WRITE to debug\out\.
setlocal
cd /d "%~dp0"
set R=%~dp0debug\out\diag.txt
if not exist "%~dp0debug" mkdir "%~dp0debug"
if not exist "%~dp0debug\out" mkdir "%~dp0debug\out"

> "%R%" echo == NTVDMEX diag  %DATE% %TIME%
>> "%R%" echo folder: %~dp0
>> "%R%" echo.
>> "%R%" echo -- 1. which Windows
ver >> "%R%" 2>&1
>> "%R%" echo.
>> "%R%" echo -- 2. is Windows' own VDM where it should be
if exist "%SystemRoot%\system32\ntvdm.exe" (
  >> "%R%" echo yes: %SystemRoot%\system32\ntvdm.exe
) else (
  >> "%R%" echo NO -- %SystemRoot%\system32\ntvdm.exe is MISSING
)
>> "%R%" echo.
>> "%R%" echo -- 3. can we write to debug\out\  [case C]
> "%~dp0debug\out\writetest.tmp" echo write test
if exist "%~dp0debug\out\writetest.tmp" (
  >> "%R%" echo yes: debug\out\ is writable
  del /q "%~dp0debug\out\writetest.tmp" >nul 2>&1
) else (
  >> "%R%" echo NO -- debug\out\ is NOT writable. That alone explains "no logs".
)
>> "%R%" echo.
>> "%R%" echo -- 4. what NTVDMEX thinks it is  (exit code: 0=ours 1=nobody 2=someone else)
"%~dp0bin\ntvdmhost.exe" /status >> "%R%" 2>&1
>> "%R%" echo /status exit code = %ERRORLEVEL%
>> "%R%" echo.
>> "%R%" echo -- 5. launching a DOS program (bin\selftest.com)
del /q "%~dp0debug\out\ntvdmhost.log" >nul 2>&1
>> "%R%" echo log deleted; launching...
start "" "%~dp0bin\selftest.com"
rem ~10 seconds, without needing a command 2000 does not have
ping -n 11 127.0.0.1 >nul 2>&1
>> "%R%" echo.
>> "%R%" echo -- 6. WHILE it is running, who is hosting it?  [case A]
>> "%R%" echo    ("copies of Windows' own ntvdm.exe" below counts the STOCK VDM;
>> "%R%" echo     NTVDMEX itself is ntvdmhost.exe and is never counted there.)
"%~dp0bin\ntvdmhost.exe" /status >> "%R%" 2>&1
>> "%R%" echo.
>> "%R%" echo -- 7. did a log appear?  [case B if not]
if exist "%~dp0debug\out\ntvdmhost.log" (
  >> "%R%" echo YES -- NTVDMEX ran. Its first lines:
  findstr /C:"STAGE0: os=" /C:"STAGE0: root" /C:"STAGE0: WinMain" /C:"STAGE1: v86_init" /C:"STAGE1: program" /C:"STAGE2: exec loop" /C:"HOSTFAULT" "%~dp0debug\out\ntvdmhost.log" >> "%R%" 2>&1
) else (
  >> "%R%" echo NO LOG. Either Windows never launched NTVDMEX [A], or it died
  >> "%R%" echo before writing a line [B]. Section 6 above tells you which.
)
>> "%R%" echo.
>> "%R%" echo -- 8. everything in debug\out\
dir "%~dp0debug\out" >> "%R%" 2>&1
>> "%R%" echo == end
echo.
echo Wrote debug\out\diag.txt -- send that file back.
echo.
echo A self-test window may still be open: it waits for a keypress at the end.
echo Press a key IN THAT WINDOW to close it. What it showed matters -- if you saw
echo the eight PASS lines, a DOS program IS running, and the question is only
echo which VDM ran it.
echo.
pause
