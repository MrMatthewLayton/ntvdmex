@echo off
rem dpmiauto.bat -- per-boot DPMI test runner (installed to C:\ntvdmex by dpmiinstall.bat,
rem launched every login via a Run registry key). Fully headless: if the DPMI test CD is
rem present it runs the test; ntvdmhost logs to COM1 -> vm/serial.log on the host, which is
rem read directly. No GUI navigation, no screendumps. A spinning PM guest is left to be
rem cleared by the NEXT `system_reset` (its diagnostics are already flushed to serial).
rem Wait for the DPMI CD to become readable: after a QMP hot-swap + hard reset, XP
rem autologin can beat the optical drive's media re-enumeration, so D:\ looks empty
rem for the first several seconds. Poll up to ~40s (20 * ~2s ping) before giving up.
set tries=0
:waitcd
if exist D:\dpmitest.com goto :haveit
set /a tries+=1
if %tries% geq 20 goto :eof
ping -n 3 127.0.0.1 >nul
goto :waitcd
:haveit
set N=C:\ntvdmex
if not exist %N% md %N%
rem IFEO Debugger -> the CD's ntvdmhost.exe directly (no stale-copy; unique CD label
rem defeats XP's CD cache).
reg add "HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Image File Execution Options\ntvdm.exe" /v Debugger /t REG_SZ /d "D:\ntvdmhost.exe" /f >nul
copy /y D:\dpmitest.com %N%\ >nul
echo %N%\dpmitest.com> %N%\target.txt
if exist %N%\ntvdmhost.log del /f /q %N%\ntvdmhost.log
rem External killer: a spinning PM guest can't self-terminate; kill from outside after
rem ~7s so a fast run's ntvdmhost exits and flushes, and the desktop is left usable.
start "" /b cmd /c "ping -n 8 127.0.0.1 >nul & taskkill /f /im ntvdmhost.exe >nul 2>&1"
start "" "D:\dosstub.com"
