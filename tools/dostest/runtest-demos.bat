@echo off
rem NTVDMEX demo runner -- run from the XP DESKTOP. Copies the host + the QBasic
rem demo EXEs off the CD, points the IFEO at ntvdmhost, and lets you run any demo
rem in a loop. Each opens a Luna window; close the window to return to this menu.
setlocal
set N=C:\ntvdmex
if not exist %N% md %N%

echo === Copying host + demos from the CD ===
copy /y "%~dp0ntvdmhost.exe" %N%\ >nul
copy /y "%~dp0dosstub.com"   %N%\ >nul
copy /y "%~dp0*.exe"         %N%\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "%N%\ntvdmhost.exe" /f >nul

:menu
echo.
echo  ===== NTVDMEX demos =====
echo   1 BLIT       2 BOUNCEBX   3 BUBBLES    4 CAVE      5 GFXCOPY
echo   6 MATRIX_1   7 MATRIX_2   8 MOUSE      9 PALETTE  10 VS87
echo   Q quit
set CH=
set /p CH=Pick a number (or Q):
if /i "%CH%"=="Q" goto end
set PROG=
if "%CH%"=="1"  set PROG=BLIT.EXE
if "%CH%"=="2"  set PROG=BOUNCEBX.EXE
if "%CH%"=="3"  set PROG=BUBBLES.EXE
if "%CH%"=="4"  set PROG=CAVE.EXE
if "%CH%"=="5"  set PROG=GFXCOPY.EXE
if "%CH%"=="6"  set PROG=MATRIX_1.EXE
if "%CH%"=="7"  set PROG=MATRIX_2.EXE
if "%CH%"=="8"  set PROG=MOUSE.EXE
if "%CH%"=="9"  set PROG=PALETTE.EXE
if "%CH%"=="10" set PROG=VS87.EXE
if "%PROG%"=="" goto menu

echo %N%\%PROG%> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
echo Running %PROG% ... close the Luna window, then close Notepad to continue.
start /wait %N%\dosstub.com
start /wait notepad %N%\ntvdmhost.log
goto menu

:end
echo Last log:
type %N%\ntvdmhost.log 2>nul
endlocal
