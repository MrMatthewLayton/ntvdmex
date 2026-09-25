@echo off
rem barelaunch.bat -- THE PRODUCT TEST: run ntvdmhost.exe with no arguments, exactly
rem   as a double-click or a shortcut does, and see whether a DOS prompt comes up.
rem   Nothing in cfg\ is set: no target.txt, no shell.txt, no dosver.txt, no int53.txt.
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%SH%\debug\out
set CFG=%SH%\cfg
set R=%OUT%\barelaunch.txt
del /q "%R%" >nul 2>&1
del /q "%OUT%\ntvdmhost.log" >nul 2>&1
del /q "%OUT%\shot*.bmp" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
if exist "%CFG%\target.txt" move /y "%CFG%\target.txt" "%CFG%\target.saved" >nul
del /q "%CFG%\autoexit" "%CFG%\int53.txt" "%CFG%\dosver.txt" "%CFG%\shell.txt" >nul 2>&1
echo 3000> "%CFG%\capture.flag"
echo 1> "%CFG%\dostrace.flag"
echo 20> "%CFG%\qimode.txt"
rem type `ver` then `dir` once the prompt is up
echo w9000 2f 12 13 1c w3000 20 17 13 1c w5000> "%CFG%\keys.txt"

echo == bare launch, NOTHING in cfg\ naming a program > "%R%"
cd /d "%SH%"
start "" "%SH%\bin\ntvdmhost.exe"
ping -n 25 127.0.0.1 >nul
echo --- processes: >> "%R%"
tasklist /fi "imagename eq ntvdmhost.exe" >> "%R%" 2>&1
echo --- what it did: >> "%R%"
findstr /C:"STAGE1: v86_init" /C:"launched with no program" /C:"loading a SHELL" /C:"NTVDM BOP sites" /C:"DOS version reported" /C:"AH=53h answers" /C:"AH=0A line" /C:"embedded fallback" "%OUT%\ntvdmhost.log" >> "%R%" 2>&1
for %%f in ("%OUT%\shot*.bmp") do copy /y "%%f" "%OUT%\shot_bare_%%~nxf" >nul 2>&1
taskkill /f /im ntvdmhost.exe >nul 2>&1
copy /y "%OUT%\ntvdmhost.log" "%OUT%\barelaunch_host.log" >nul 2>&1
del /q "%CFG%\capture.flag" "%CFG%\qimode.txt" "%CFG%\keys.txt" "%CFG%\dostrace.flag" >nul 2>&1
if exist "%CFG%\target.saved" move /y "%CFG%\target.saved" "%CFG%\target.txt" >nul
echo --- done, cfg restored >> "%R%"
