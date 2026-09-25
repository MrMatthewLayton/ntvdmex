@echo off
rem shelltest.bat -- the PRODUCT shape: a plain Win32 launch of a DOS image, with
rem   NOTHING naming a program. If the host's last-resort shell branch works, a
rem   real DOS prompt should come up and STAY up (no cfg\autoexit).
rem   %1 = "knobs" to also set dosver 5.0 + int53 (what XP's COMMAND.COM needs).
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set R=%OUT%\shelltest.txt
del /q "%R%" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\shot*.bmp" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1

rem -- NOTHING must name a program: move target.txt out of the way.
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.saved" >nul
del /q "%CFG%\autoexit" >nul 2>&1
del /q "%CFG%\int53.txt" >nul 2>&1
del /q "%CFG%\dosver.txt" >nul 2>&1
if /i "%1"=="knobs" (
  copy /y "%SH%\debug\rig\int53-interactive.txt" "%CFG%\int53.txt" >nul
  echo 5.0> "%CFG%\dosver.txt"
)
echo 2000> "%CFG%\capture.flag"

echo == launching dosstub.com with no target.txt, knobs=%1 > "%R%"
cd /d "%SH%\debug\rig"
start "" "%SH%\debug\rig\dosstub.com"
ping -n 20 127.0.0.1 >nul
echo --- processes: >> "%R%"
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
echo --- what the host decided: >> "%R%"
findstr /C:"STAGE2: target.txt" /C:"STAGE2: CSRSS" /C:"shell" /C:"SHELL" /C:"STAGE2: loaded" /C:"STAGE2: running" /C:"DOS version reported" /C:"AH=53h answers" /C:"embedded fallback" /C:"STAGE1: v86_init" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_shelltest_%%~nxf" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
del /q "%CFG%\capture.flag" >nul 2>&1
del /q "%CFG%\int53.txt" >nul 2>&1
del /q "%CFG%\dosver.txt" >nul 2>&1
if exist "%CFG%\target.saved" move /y "%CFG%\target.saved" "%CFG%\target.txt" >nul
echo --- done, cfg restored >> "%R%"
