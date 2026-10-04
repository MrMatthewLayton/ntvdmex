@echo off
rem w16stockdrive.bat <Folder> <EXE> <script> -- w16drive.bat's steps, under STOCK ntvdm
rem (s92). The reference for anything a still photograph cannot show: a live resize,
rem a window dragged over another. Same step file as w16drive.bat (debug\rig\<script>.txt).
rem Report: debug\out\w16sdrive_<script>.txt.
rem ⛔ THE IFEO KEY IS DROPPED FOR THE RUN AND RESTORED UNCONDITIONALLY, exactly as
rem   w16stockshot.bat does it; the caller FAILS LOUDLY unless the restore is proven.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set OUT=%SH%\debug\out
set W16=%SH%\demo\win16
set IFEO=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe
set R=%OUT%\w16sdrive_%3.txt
set S=%OUT%\w16stock_state.txt
del /q "%SH%\w16stock_done.txt" >nul 2>&1
> "%R%" echo == w16stockdrive %1 %2 %3  %DATE% %TIME%
if not exist "%W16%\%1\%2" (
  >> "%R%" echo NO SUCH APP
  echo NO SUCH APP > "%S%"
  echo done > "%SH%\w16stock_done.txt"
  goto :eof
)
taskkill /f /im ntvdmhost.exe >nul 2>&1
taskkill /f /im ntvdm.exe     >nul 2>&1
echo ---- IFEO before ---- > "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
reg delete "%IFEO%" /v Debugger /f >nul 2>&1
echo ---- IFEO removed, running STOCK ---- >> "%S%"
cd /d "%W16%\%1"
start "" "%W16%\%1\%2"
ping -n 10 127.0.0.1 >nul
for /f "usebackq delims=" %%L in ("%RIG%\%3.txt") do call :step %%L
taskkill /f /im ntvdm.exe >nul 2>&1
reg add "%IFEO%" /v Debugger /t REG_SZ /d "\"%BIN%\ntvdmhost.exe\"" /f >nul
echo ---- IFEO after ---- >> "%S%"
reg query "%IFEO%" /v Debugger >> "%S%" 2>&1
if exist "%BIN%\ntvdmhost.exe" (
  echo ---- restored target EXISTS ---- >> "%S%"
) else (
  echo ---- ****** RESTORED TARGET IS MISSING -- THE RIG IS BROKEN ****** ---- >> "%S%"
)
>> "%R%" echo == done %TIME%
echo done > "%SH%\w16stock_done.txt"
goto :eof

:step
>> "%R%" echo ^> %*
if /i "%~1"=="wait" ( ping -n %2 127.0.0.1 >nul & goto :eof )
if /i "%~1"=="shot" ( "%RIG%\rigshot.exe" shot "%OUT%\%~2_stock.bmp" >nul 2>&1 & goto :eof )
del /q "%SH%\debug\ctl\rigshot.txt" >nul 2>&1
if /i "%~1"=="list" ( "%RIG%\rigshot.exe" list ) else ( "%RIG%\rigshot.exe" %* )
ping -n 2 127.0.0.1 >nul
type "%SH%\debug\ctl\rigshot.txt" >> "%R%" 2>&1
goto :eof
