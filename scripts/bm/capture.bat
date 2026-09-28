@echo off
rem capture.bat -- #155 Tools > Capture from the menu: Record Audio on (22), a screenshot (8),
rem Record Audio off (22). Report: debug\out\capture.txt -- the files it made, and the log lines.
setlocal
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BIN=%SH%\bin
set RIG=%SH%\debug\rig
set CFG=%SH%\cfg
set OUT=%SH%\debug\out
set R=%RIG%\rigshot.exe
set L=%SH%\debug\ctl\rigshot.txt
set REP=%OUT%\capture.txt
tasklist | find /i "ntvdmhost" >nul
if not errorlevel 1 ( echo ABORT: an NTVDMEX is already running -- not touching it> "%REP%" & goto :eof )
> "%REP%" echo == capture %DATE% %TIME%
del /q "%OUT%\capture_audio_*.wav" "%OUT%\shot_manual_*.bmp" "%L%" >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.txt.capture" >nul
start "" /D "%SH%" "%BIN%\ntvdmhost.exe"
ping -n 10 127.0.0.1 >nul
"%R%" cmd 22
ping -n 4 127.0.0.1 >nul
"%R%" cmd 8
ping -n 2 127.0.0.1 >nul
"%R%" cmd 22
ping -n 3 127.0.0.1 >nul
copy /y "%OUT%\ntvdmhost.log" "%OUT%\capture_host.log" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
if exist "%CFG%\target.txt.capture" move /y "%CFG%\target.txt.capture" "%CFG%\target.txt" >nul
type "%L%" >> "%REP%" 2>&1
dir /b "%OUT%\capture_audio_*.wav" "%OUT%\shot_manual_*.bmp" >> "%REP%" 2>&1
find "audio recording" "%OUT%\capture_host.log" >> "%REP%" 2>&1
>> "%REP%" echo == done %TIME%
endlocal
