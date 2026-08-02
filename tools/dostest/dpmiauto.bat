@echo off
rem dpmiauto.bat -- per-boot DPMI test runner (installed to C:\ntvdmex by dpmiinstall.bat,
rem launched every login via a Run registry key). Fully headless: runs the DPMI gate off the
rem mounted test CD; ntvdmhost logs to COM1 -> vm/serial.log on the host, read directly. No
rem GUI, no screendumps. A spinning PM guest is cleared by the NEXT system_reset.
rem
rem Self-refreshing: copies the CD's own dpmiauto.bat over the installed one first, so future
rem edits to this file take effect on the next boot without re-running dpmiinstall.bat.
rem
rem Runs TWO clients in sequence, both logging to the same serial:
rem   dpmitest.com  -- the .COM DPMI client (CS=DS=SS=PSP): descriptors, DOS+ext mem, file I/O,
rem                    PM vectors, 0300 sim-int, 0301 far-call, 0303 callback w/ nested INTs.
rem   dpmiexe.exe   -- a real multi-segment MZ .EXE (CS!=DS!=SS): proves the .EXE load path +
rem                    the three-selector switch (GH #2, run 48).
set N=C:\ntvdmex
if not exist %N% md %N%
if exist D:\dpmiauto.bat copy /y D:\dpmiauto.bat %N%\dpmiauto.bat >nul
rem Wait for the DPMI CD to become readable: after a QMP hot-swap + hard reset, XP autologin
rem can beat the optical drive's media re-enumeration, so D:\ looks empty for several seconds.
set tries=0
:waitcd
if exist D:\dpmitest.com goto :haveit
set /a tries+=1
if %tries% geq 20 goto :eof
ping -n 3 127.0.0.1 >nul
goto :waitcd
:haveit
rem IFEO Debugger -> the CD's ntvdmhost.exe directly (unique CD label defeats XP's CD cache).
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "D:\ntvdmhost.exe" /f >nul
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
call :runone dpmitest.com
call :runone dpmiexe.exe
goto :eof

:runone
rem %1 = a client binary present on the CD. Copy it in, point target.txt at it, trigger the
rem host, and wait for it to finish (an external killer bounds a spinning PM guest at ~8s).
if not exist D:\%1 goto :eof
copy /y D:\%1 %N%\ >nul
echo %N%\%1> %N%\target.txt
start "" /b cmd /c "ping -n 8 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start "" /wait "D:\dosstub.com"
ping -n 2 127.0.0.1 >nul
goto :eof
