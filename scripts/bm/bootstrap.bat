@echo off
rem ============================================================================
rem  ONE-SHOT BOOTSTRAP back to a working rig, staged as <share>\doomrun.bat.
rem
rem  WHY THAT NAME.  After the wipe the running watcher is still the OLD one, and
rem  it invokes C:\WINDOWS\rt.bat -- also the old copy.  The one hook the old
rem  rt.bat offers into an arbitrary script is its `doom` arm, which calls
rem      call "<share>\doomrun.bat"
rem  by absolute path.  So queueing `doom` runs THIS, and the box can be repaired
rem  without anyone standing at it and without a reboot.
rem
rem  It installs the new layout, then leaves a BRIDGE: the new rt.bat is copied to
rem  C:\WINDOWS\rt.bat so the still-running old watcher keeps working.  That last
rem  file is deliberate and temporary -- `rt.bat clean` removes it once the box has
rem  been rebooted onto the new watcher, which calls bm\rt.bat directly.
rem ============================================================================
set SH=C:\Documents and Settings\All Users\Documents\ntvdmex
set BM=%SH%\bm
set OUT=%SH%\out
set R=%SH%\result_doom.log

if not exist "%SH%\cfg" md "%SH%\cfg"
if not exist "%OUT%" md "%OUT%"

echo NTVDMEX BOOTSTRAP > "%R%"
echo. >> "%R%"

echo === BEFORE === >> "%R%"
if exist C:\ntvdmex        echo FOUND C:\ntvdmex        >> "%R%"
if exist C:\test           echo FOUND C:\test           >> "%R%"
if exist C:\game           echo FOUND C:\game           >> "%R%"
if exist C:\DOOMS          echo FOUND C:\DOOMS          >> "%R%"
if exist C:\WINDOWS\rt.bat echo FOUND C:\WINDOWS\rt.bat >> "%R%"
echo old IFEO value: >> "%R%"
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger >> "%R%" 2>&1

rem -- 1. THE HOST MUST EXIST WHERE THE KEY POINTS.  A Debugger value naming a
rem       deleted binary makes EVERY 16-bit launch on the box fail, which is what
rem       "nothing ran at all" was.
taskkill /f /im ntvdmhost.exe >nul 2>&1
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "\"%BM%\ntvdmhost.exe\"" /f >nul 2>&1

rem -- 2. New watcher into Startup (takes effect at the next reboot).
copy /y "%BM%\runwatch.bat" "%ALLUSERSPROFILE%\Start Menu\Programs\Startup\ntvdmex-watch.bat" >nul 2>&1

rem -- 3. BRIDGE for the watcher that is running RIGHT NOW.
copy /y "%BM%\rt.bat" C:\WINDOWS\rt.bat >nul 2>&1

rem -- 4. Remove the old layout's artefacts.
rmdir /s /q C:\ntvdmex >nul 2>&1
rmdir /s /q C:\test    >nul 2>&1
rmdir /s /q C:\game    >nul 2>&1
del /q "%SH%\*.flag" >nul 2>&1
del /q "%SH%\*.bmp"  >nul 2>&1

echo. >> "%R%"
echo === AFTER === >> "%R%"
if exist C:\ntvdmex        echo STILL C:\ntvdmex        >> "%R%"
if exist C:\test           echo STILL C:\test           >> "%R%"
if exist C:\game           echo STILL C:\game           >> "%R%"
echo new IFEO value: >> "%R%"
reg query "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger >> "%R%" 2>&1
echo host binary: >> "%R%"
dir "%BM%\ntvdmhost.exe" >> "%R%" 2>&1
echo. >> "%R%"
echo --- C:\ root --- >> "%R%"
dir /b C:\ >> "%R%" 2>&1
echo. >> "%R%"
echo --- share root --- >> "%R%"
dir /b "%SH%" >> "%R%" 2>&1
echo. >> "%R%"
echo --- games --- >> "%R%"
dir /b "%SH%\games" >> "%R%" 2>&1
copy /y "%R%" "%OUT%\result_bootstrap.log" >nul 2>&1
