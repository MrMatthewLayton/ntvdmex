@echo off
rem cavelive.bat -- launch CAVE.EXE under NTVDMEX and LEAVE IT RUNNING, so the View
rem menu and the CPU Speed dropdown can be exercised by hand against something that
rem is actually moving (GH #56).
rem
rem ⚠ FOUR THINGS MAKE THIS DIFFERENT FROM EVERY OTHER RIG BATCH, and each of them
rem   would otherwise end the session the batch is trying to start:
rem
rem   1. NO `autoexit` MARKER. That file is what puts the host in headless mode, and
rem      headless mode starts a deadline thread that clears g_running after
rem      PM_HEADLESS_MS and then force-exits. It is deleted, not merely not created.
rem
rem   2. NO `/wait` ON start. rt.bat blocks in `start /wait` until the guest exits --
rem      right for a measured run, and it would wedge runwatch.bat forever for a run
rem      that is meant to outlive the batch.
rem
rem   3. ⚠⚠ THE COPY IS VERIFIED, BECAUSE IT SILENTLY FAILED ONCE AND COST A WHOLE
rem      GATE RUN. `taskkill` returns BEFORE Windows has released the image handle,
rem      so a `copy /y` issued immediately afterwards fails -- and with its output
rem      sent to nul it fails invisibly, leaving the PREVIOUS binary in place while
rem      everything about the run looks normal. That is the project's standing
rem      "deployed the wrong exe" trap in a new costume, and the only reason it was
rem      caught is that the status bar happened to be one of the things under test.
rem      So: wait, copy LOUDLY, and print the size of what is on disk beside the size
rem      of what we meant to deploy. They must match.
rem
rem   4. CAVE WAITS FOR A KEYPRESS before it draws anything. A black client area here
rem      is not a defect and not a slow start -- the demo is waiting. Send it a key,
rem      or the run reads as "the host loaded it and it never rendered".
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%RES%\bm
set K=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe

taskkill /f /im ntvdmhost.exe >nul 2>&1
tskill ntvdmhost >nul 2>&1
rem ⚠ ntvdm.exe too: XP's WOW VDM is shared, so a stale one silently steals the
rem   launch and the IFEO hook never fires.
taskkill /f /im ntvdm.exe >nul 2>&1
ping -n 4 127.0.0.1 >nul

del /q "%RES%\cavelive.txt" >nul 2>&1
del /q "%RES%\cavelive_done.txt" >nul 2>&1
if not exist C:\ntvdmex md C:\ntvdmex
copy /y "%BM%\ntvdmhost.exe" C:\ntvdmex\ > "%RES%\cavelive.txt" 2>&1
echo ---- deployed (C:\ntvdmex) vs staged (share): SIZES MUST MATCH ---- >> "%RES%\cavelive.txt"
dir /-c C:\ntvdmex\ntvdmhost.exe | findstr ntvdmhost >> "%RES%\cavelive.txt"
dir /-c "%BM%\ntvdmhost.exe"     | findstr ntvdmhost >> "%RES%\cavelive.txt"

rem ⚠ The IFEO key is re-added every time even though it should already be there:
rem   GH #132 drops it after three unclean starts, and every batch that opens with
rem   taskkill is manufacturing unclean starts. A launch that quietly ran under stock
rem   ntvdm would look exactly like a feature that does nothing.
reg add "%K%" /v Debugger /t REG_SZ /d "C:\ntvdmex\ntvdmhost.exe" /f >nul

rem Hand it over at the baseline: 1x window, no scaler, Unlimited speed. Anything
rem changed from the View menu after this is session-only and will not stick.
reg add "HKCU\Software\NTVDMEX" /v WindowSize /t REG_DWORD /d 0 /f >nul
reg add "HKCU\Software\NTVDMEX" /v Scaler     /t REG_DWORD /d 0 /f >nul
reg add "HKCU\Software\NTVDMEX" /v SpeedMode  /t REG_DWORD /d 0 /f >nul
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
ping -n 11 127.0.0.1 >nul

rem The keypress CAVE is waiting for.
rem ⚠⚠ CLICK THE CLIENT AREA FIRST. A synthetic key sent straight after
rem   SetForegroundWindow lands on the MENU BAR, not on the guest: Enter opened the
rem   Settings dialog the first time this ran, and space opened the View menu the
rem   second. Both times the guest got its key as well -- so it drew, and it drew
rem   UNDERNEATH a menu, which reads as "the run left a menu open" rather than as the
rem   input going somewhere unintended. Clicking in the picture puts the focus where
rem   the keystroke is meant to go. CAVE has no mouse handling, so the click itself
rem   is inert.
"%BM%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
ping -n 2 127.0.0.1 >nul
"%BM%\rigshot.exe" click 496 450 >nul 2>&1
ping -n 2 127.0.0.1 >nul
"%BM%\rigshot.exe" key 0x20 >nul 2>&1
ping -n 5 127.0.0.1 >nul

echo ---- is it up, and is it OURS? ---- >> "%RES%\cavelive.txt"
tasklist /fi "imagename eq ntvdmhost.exe" | find "ntvdmhost" >> "%RES%\cavelive.txt"
if errorlevel 1 echo ^*^*^* NTVDMHOST IS NOT RUNNING >> "%RES%\cavelive.txt"
if not exist C:\ntvdmex\ntvdmhost.log echo ^*^*^* NO HOST LOG -- stock ntvdm ran this >> "%RES%\cavelive.txt"
findstr /c:"target.txt loaded" C:\ntvdmex\ntvdmhost.log >> "%RES%\cavelive.txt" 2>&1
echo ---- windows on the desktop ---- >> "%RES%\cavelive.txt"
"%BM%\rigshot.exe" list >nul 2>&1
type "%RES%\rigshot.txt" >> "%RES%\cavelive.txt" 2>&1

rem TWO shots a second apart: one picture cannot tell a running animation from a
rem frozen frame, and "is it advancing" is the whole question the speed dropdown asks.
"%BM%\rigshot.exe" shot "%RES%\cave_a.bmp" >nul 2>&1
ping -n 2 127.0.0.1 >nul
"%BM%\rigshot.exe" shot "%RES%\cave_b.bmp" >nul 2>&1

rem Leave the guest's window in front, ready for a human.
"%BM%\rigshot.exe" fg "Microsoft Windows XP Virtual DOS Machine" >nul 2>&1
echo done> "%RES%\cavelive_done.txt"
endlocal
