@echo off
rem gpuinfo.bat -- which display adapter and driver does this box have? (s86)
rem
rem Asked because DirectDraw fullscreen tears far more than GDI on the rig: our GDI
rem path times its own blit to the vertical blank, the DirectDraw path trusts the
rem DRIVER's Flip to do it. Whether that flip waits depends on which driver this is.
rem Read-only: the display class key, plus dxdiag's text report (DirectDraw section).
setlocal
set RES=C:\Documents and Settings\All Users\Documents\ntvdmex
set OUT=%RES%\debug\out\gpuinfo.txt
del /q "%OUT%" >nul 2>&1
del /q "%RES%\debug\out\gpuinfo_dx.txt" >nul 2>&1
del /q "%RES%\debug\out\gpuinfo_done.txt" >nul 2>&1

echo ==== display class (driver, version, provider) ====> "%OUT%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\Class\{4D36E968-E325-11CE-BFC1-08002BE10318}" /s >> "%OUT%" 2>&1
echo.>> "%OUT%"
echo ==== DirectDraw global settings ====>> "%OUT%"
reg query "HKLM\SOFTWARE\Microsoft\DirectDraw" >> "%OUT%" 2>&1

start /wait dxdiag /t "%RES%\debug\out\gpuinfo_dx.txt"
echo done> "%RES%\debug\out\gpuinfo_done.txt"
