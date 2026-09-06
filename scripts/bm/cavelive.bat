@echo off
rem cavelive.bat -- launch CAVE.EXE under NTVDMEX and LEAVE IT RUNNING, so the CPU
rem Speed dropdown can be exercised by hand while something is animating (GH #56).
rem
rem ⚠ THREE THINGS MAKE THIS DIFFERENT FROM EVERY OTHER RIG BATCH, and each of them
rem   would otherwise end the session the batch is trying to start:
rem   1. NO `autoexit` MARKER. That file is what puts the host in headless mode, and
rem      headless mode starts a deadline thread that clears g_running after
rem      PM_HEADLESS_MS and then force-exits. It is deleted, not merely not created.
rem   2. NO `/wait` ON start. rt.bat blocks in `start /wait` until the guest exits --
rem      which is exactly right for a measured run and would wedge runwatch.bat
rem      forever for a run that is meant to outlive the batch.
rem   3. SpeedMode IS RESET TO 0 (Unlimited) FIRST, so the machine starts at the
rem      baseline and every step down is a change the user can feel against it. That
rem      registry value used to be a dead combo; anything left in it from before now
rem      means a speed.
rem
rem ⚠ The IFEO key is re-added here even though it should already be present: GH #132
rem   drops it after three unclean starts, and every batch that begins with taskkill
rem   is manufacturing unclean starts. A launch that silently runs under stock ntvdm
rem   would look exactly like a throttle that does nothing.
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
rem ⚠ ntvdm.exe too: XP's VDM is shared, so a stale one silently steals the launch
rem   and the IFEO hook never fires.
taskkill /f /im ntvdm.exe >nul 2>&1

if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ >nul
reg add "%K%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul

rem Start at Unlimited, and clear the one-run override file so the registry decides.
reg add "HKCU\Software\NTVDMEX" /v SpeedMode /t REG_DWORD /d 0 /f >nul
del /q "%RES%\cpuspd.txt" >nul 2>&1

if not exist C:\test md C:\test
copy /y "%BM%\dosstub.com" C:\test\ >nul
copy /y "%BM%\tests\CAVE.EXE" C:\test\ >nul
echo C:\test\CAVE.EXE> C:\ntvdmex\target.txt
del /q C:\ntvdmex\autoexit >nul 2>&1
del /q C:\ntvdmex\ntvdmhost.log >nul 2>&1
del /q "%RES%\rigshot.txt" >nul 2>&1

cd /d C:\test
start "" C:\test\dosstub.com

rem Give it time to load, enter V86 and put a window up before looking.
ping -n 10 127.0.0.1 >nul

del /q "%RES%\cavelive.txt" >nul 2>&1
echo ==== is CAVE up under OUR host? ====> "%RES%\cavelive.txt"
rem ⚠ `tasklist | find` -- a bare tasklist prints "INFO: No tasks running" and still
rem   returns success, which is the same do-nothing-and-succeed shape that has fooled
rem   this project before. Name what we expect to see.
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >> "%RES%\cavelive.txt"
if errorlevel 1 echo ^*^*^* NTVDMHOST IS NOT RUNNING >> "%RES%\cavelive.txt"
tasklist /fi "imagename eq ntvdm.exe" | find "ntvdm.exe" >> "%RES%\cavelive.txt"
echo ---- host log ---- >> "%RES%\cavelive.txt"
findstr /c:"STAGE1: program" /c:"STAGE2: cpuspeed" /c:"STAGE1: video" C:\ntvdmex\ntvdmhost.log >> "%RES%\cavelive.txt" 2>&1
echo ---- windows on the desktop ---- >> "%RES%\cavelive.txt"
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%RES%\cavelive.txt" 2>&1
rem `shot` takes the output path as an argument -- it does not pick one.
"%BM%\rigshot.exe" shot "%RES%\cave_live.bmp" >nul 2>&1
echo done> "%RES%\cavelive_done.txt"
endlocal
