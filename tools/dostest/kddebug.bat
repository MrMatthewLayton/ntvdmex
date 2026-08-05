@echo off
rem kddebug.bat -- enable XP kernel debugging on COM2 (option B: r2 winkd observation of
rem the VDM #GP-reflect). One double-click appends the debug switches to boot.ini's OS line
rem so we don't have to type them. Reboot to activate, then on the host:
rem   r2 winkd://vm/kd.sock     (QEMU exposes guest COM2 as vm/kd.sock)
setlocal
echo Enabling kernel debugging on COM2 @115200...
bootcfg /raw "/debug /debugport=COM2 /baudrate=115200" /a /id 1
echo.
echo --- current boot configuration: ---
bootcfg /query
echo.
echo If the OS entry above now shows /debug /debugport=COM2, reboot to activate KD.
echo Then attach from the host with:  r2 winkd://vm/kd.sock
pause
endlocal
