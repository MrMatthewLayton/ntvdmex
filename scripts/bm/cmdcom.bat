@echo off
rem cmdcom.bat -- run XP's OWN 16-bit COMMAND.COM as a guest under NTVDMEX.
rem
rem   cmdcom.bat            -> C:\WINDOWS\system32\command.com, cwd C:\
rem   cmdcom.bat D:\DOS     -> a different working directory
rem
rem WHY THIS IS A GOOD TEST AND NOT A DEMO. Every game so far is a single program that
rem takes over the machine; a SHELL is the other shape entirely. It stays resident, reads
rem a line at a time through INT 21h, walks directories (4Eh/4Fh), and EXECs child
rem processes (4Bh) that must return to it. That is a broad, boring slice of the DOS API
rem which no game exercises -- exactly the M9 completeness surface.
rem
rem HEADLESS: keys.txt types the commands, capture.flag takes the screenshots. There is
rem nobody at the keyboard, so a shell that comes up and waits looks identical to a shell
rem that has hung -- the SHOTS are what tell them apart.
setlocal
set SH=%~dp0
set G=%1
if "%G%"=="" set G=C:\
set N=C:\ntvdmex

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist %N% md %N%
copy /y "%SH%bm\ntvdmhost.exe" %N%\ >nul
copy /y "%SH%bm\dosstub.com" "%G%" >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul

>%N%\target.txt echo C:\WINDOWS\system32\command.com
echo.> %N%\autoexit

rem -- DELETE THE DESTINATION FIRST. A stale shot or log from the last run reads as a
rem    result; absent reads as a failure. See the sbcopy note in doomrun.bat.
del /q %N%\shot*.bmp >nul 2>&1
del /q "%SH%shot_cmdcom_*.bmp" >nul 2>&1
del /q "%SH%result_cmdcom.log" >nul 2>&1
del /q %N%\ntvdmhost.log >nul 2>&1

cd /d "%G%"
start /wait /d "%G%" "" "%G%\dosstub.com"

rem -- BRING THE GUEST BINARY BACK. Twice now the answer has come from disassembling
rem    the guest rather than instrumenting the host (sessions 21 and 25), and a shell
rem    that exits without printing is exactly that shape of question. The share is the
rem    only channel off this box, so copy it out once and it can be read at leisure.
copy /y C:\WINDOWS\system32\command.com "%SH%guest\command.com" >nul 2>&1

copy /y %N%\ntvdmhost.log "%SH%result_cmdcom.log" >nul 2>&1
for %%f in (%N%\shot*.bmp) do copy /y "%%f" "%SH%shot_cmdcom_%%~nxf" >nul 2>&1
del /q %N%\autoexit >nul 2>&1
echo Done. Log -> result_cmdcom.log, shots -> shot_cmdcom_*.bmp
endlocal
