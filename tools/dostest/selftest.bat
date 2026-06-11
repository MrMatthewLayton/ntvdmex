@echo off
rem NTVDMEX one-shot regression suite -- run from the XP DESKTOP off the CD.
rem Copies the host + selftest.com to C:\ntvdmex, points the IFEO at our host,
rem and runs the whole subsystem suite in ONE VDM session. The Luna window shows
rem a PASS/FAIL report and waits for a key (so it can be screendumped); after you
rem close it, the captured ntvdmhost.log is printed here too.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

copy /y "%~dp0ntvdmhost.exe" %N%\ >nul
copy /y "%~dp0dosstub.com"   %N%\ >nul
copy /y "%~dp0selftest.com"  %N%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f >nul

echo %N%\selftest.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log

echo Running NTVDMEX self-test suite... a Luna window will show the report.
echo When it says "Press any key", read/screendump it, press a key to close.
start /wait %N%\dosstub.com

echo.
echo ============ ntvdmhost.log ============
type %N%\ntvdmhost.log 2>nul
echo =======================================
echo Self-test errorlevel (= number of failed tests): %ERRORLEVEL%
endlocal
