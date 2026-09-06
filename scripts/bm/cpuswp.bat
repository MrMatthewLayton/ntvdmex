@echo off
rem cpuswp.bat -- GH #56 acceptance sweep: run cpubench.com at EVERY speed index,
rem in ONE batch. One batch because a multi-step rig test driven as separate execs
rem races itself and deletes its own evidence (session 49 paid for that lesson).
rem
rem What it produces, per index, on the share:
rem   cpuswp_<n>.log   the whole host log, so the STAGE2 cpuspeed line (duty, run_ms,
rem                    held_ms, missed) sits beside the guest's own reported MHz
rem   cpuswp.txt       the one-line-per-index summary, which is what to read first
rem
rem The number that matters is the guest's own "N MHz apparent": at index 0 it
rem calibrates CPUSPEED_REF_MHZ, and at every other index it should land near the
rem label on the menu. If it does not, the calibration is wrong, not the mechanism.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul
if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\cpubench.com" C:\test\ >nul
echo C:\test\cpubench.com> C:\ntvdmex\target.txt
del /q "%RES%\cpuswp.txt" >nul 2>&1
del /q "%RES%\cpuswp_done.txt" >nul 2>&1
del /q "%RES%\cpuswp_?.log" >nul 2>&1
echo ==== cpubench sweep, one run per speed index ====> "%RES%\cpuswp.txt"
for %%s in (0 1 2 3 4 5 6) do call :one %%s
del /q "%RES%\cpuspd.txt" >nul 2>&1
echo done> "%RES%\cpuswp_done.txt"
endlocal
goto :eof

:one
rem The speed knob is read at startup, so it must be in place BEFORE the launch.
rem ⚠ THE REDIRECT COMES FIRST, AND THAT IS NOT STYLE. `echo %1> file` with %1=2 is
rem   `echo 2> file` -- cmd reads the digit as a FILE DESCRIPTOR and redirects
rem   stderr, writing "ECHO is on." into the file. The first cut did exactly that
rem   for every index, so the whole sweep ran unthrottled and reported seven
rem   identical numbers that looked like "the throttle does nothing".
> "%RES%\cpuspd.txt" echo %1
echo.> C:\ntvdmex\autoexit
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
cd /d C:\test
start /wait "" C:\test\dosstub.com
copy /y C:\ntvdmex\ntvdmhost.log "%RES%\cpuswp_%1.log" >nul 2>&1
echo. >> "%RES%\cpuswp.txt"
echo ---- index %1 ---- >> "%RES%\cpuswp.txt"
findstr /c:"MHz apparent" /c:"iters /" /c:"STAGE2: cpuspeed" "%RES%\cpuswp_%1.log" >> "%RES%\cpuswp.txt" 2>&1
goto :eof
