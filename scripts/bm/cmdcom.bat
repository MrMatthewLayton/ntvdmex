@echo off
rem cmdcom.bat -- run a 16-bit COMMAND.COM as a guest under NTVDMEX.
rem
rem   cmdcom.bat        -> MS-DOS 6.22's COMMAND.COM, from guest\COMMAND.COM on the
rem                        share (extracted from msdos-622/Disk1.img in the repo)
rem   cmdcom.bat xp     -> XP's own C:\WINDOWS\system32\command.com
rem
rem WHY THIS IS A TEST AND NOT A DEMO. Every game so far is a single program that takes
rem over the machine; a SHELL is the other shape entirely. It stays resident, reads a
rem line at a time through INT 21h, walks directories (4Eh/4Fh) and EXECs children (4Bh)
rem that must return to it. That is a broad, boring slice of the DOS API which no game
rem exercises -- exactly the M9 completeness surface.
rem
rem WHICH ONE TO PREFER: the 6.22 build. It is the M9 oracle's own shell and it matches
rem the version we report by default, so a failure is OUR gap rather than a disagreement
rem about what DOS is. XP's is NT's shell, wants version 5.00 (see dosver.txt), and
rem leans on NTVDM-specific behaviour.
rem
rem HEADLESS: keys.txt types the commands, capture.flag takes the screenshots. There is
rem nobody at the keyboard, so a shell that comes up and waits looks identical to one
rem that has hung -- the SHOTS are what tell them apart.
setlocal
set SH=%~dp0
set N=C:\ntvdmex
set D=C:\dostest

set WHICH=%1
if "%WHICH%"=="" set WHICH=622

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist %N% md %N%
if not exist %D% md %D%
copy /y "%SH%bm\ntvdmhost.exe" %N%\ >nul
copy /y "%SH%bm\dosstub.com" %D%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul

rem -- DELETE THE DESTINATION FIRST. A stale shot or log reads as a result; absent reads
rem    as a failure. See the sbcopy note in doomrun.bat.
del /q %N%\shot*.bmp >nul 2>&1
del /q "%SH%shot_cmdcom_*.bmp" >nul 2>&1
del /q "%SH%result_cmdcom.log" >nul 2>&1
del /q %N%\ntvdmhost.log >nul 2>&1

if /i "%WHICH%"=="xp" goto xpshell
copy /y "%SH%guest\COMMAND.COM" %D%\ >nul
rem -- and a real EXTERNAL command, so AH=4Bh EXEC gets exercised: the shell must load
rem    a child, run it, and get control back. ATTRIB.EXE is small, reads the directory
rem    and exits, which is exactly the shape we want.
copy /y "%SH%guest\ATTRIB.EXE" %D%\ >nul
>%N%\target.txt echo %D%\COMMAND.COM
goto go
:xpshell
>%N%\target.txt echo C:\WINDOWS\system32\command.com
:go
echo.> %N%\autoexit
type %N%\target.txt

rem -- THE KEY SCRIPT IS GATED BEHIND qimode BIT 0x20, AND THE HAND-PLAY SETTING IS 0.
rem    Nobody is at this keyboard, so without the script the shell simply blocks at its
rem    prompt for the whole run and the log shows an idle host -- which is exactly what
rem    the first 6.22 run looked like (sc_push=0, int16=[0,0,0,0]).
rem    Set it HERE and put it back afterwards rather than leaving it on the share: the
rem    standing note says qimode must be 0 before playing by hand or the script fights
rem    you for the keyboard, and a knob left flipped by a test is a trap for the next
rem    person at the box.
set QIOLD=0
if exist "%SH%qimode.txt" for /f "delims=" %%a in ('type "%SH%qimode.txt"') do set QIOLD=%%a
>"%SH%qimode.txt" echo 20

cd /d %D%
start /wait /d "%D%" "" "%D%\dosstub.com"

>"%SH%qimode.txt" echo %QIOLD%

rem -- BRING THE GUEST BINARY BACK, AND DO NOT SWALLOW THE COPY'S OUTPUT. Twice the
rem    answer has come from disassembling the guest rather than instrumenting the host
rem    (sessions 21 and 25). The first cut of this redirected the error to nul, so a
rem    copy that never happened looked exactly like one that did -- the same mistake
rem    the sbcopy comment in doomrun.bat exists to warn about.
if not exist "%SH%guest" md "%SH%guest"
echo cmdcom: guest copy, which=%WHICH% > "%SH%guestcopy.txt"
if /i "%WHICH%"=="xp" (
  dir C:\WINDOWS\system32\command.com >> "%SH%guestcopy.txt" 2>&1
  copy /y C:\WINDOWS\system32\command.com "%SH%guest\xp-command.com" >> "%SH%guestcopy.txt" 2>&1
) else (
  echo skipped: the 6.22 shell came FROM the share >> "%SH%guestcopy.txt"
)

copy /y %N%\ntvdmhost.log "%SH%result_cmdcom.log" >nul 2>&1
for %%f in (%N%\shot*.bmp) do copy /y "%%f" "%SH%shot_cmdcom_%%~nxf" >nul 2>&1
del /q %N%\autoexit >nul 2>&1
echo Done. Log -> result_cmdcom.log, shots -> shot_cmdcom_*.bmp
endlocal
